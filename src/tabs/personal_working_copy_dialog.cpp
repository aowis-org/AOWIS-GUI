#include "tabs/personal_working_copy_dialog.h"
#include "projects/dvmdb_identity.h"
#include "projects/personal_edit_queue.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <utility>

namespace
{
// QDialogButtonBox retranslates its standard buttons when it receives its own
// LanguageChange event, which can arrive after the parent's event. Restore the
// application catalogue text from the button box itself, without replacing it.
class WorkingCopyCloseButtonBox final : public QDialogButtonBox
{
public:
    explicit WorkingCopyCloseButtonBox(QWidget *parent)
        : QDialogButtonBox(QDialogButtonBox::Close, parent) {}

protected:
    void changeEvent(QEvent *event) override
    {
        QDialogButtonBox::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
        {
            if (QPushButton *close = button(QDialogButtonBox::Close))
                close->setText(QCoreApplication::translate("PersonalWorkingCopyDialog", "Close"));
        }
    }
};
}

PersonalWorkingCopyDialog::PersonalWorkingCopyDialog(
        const QString &project_id, const QString &branch_id,
        const QString &branch_name, const QString &head,
        const QString &source_repository_id, const QString &source_endpoint,
        const QString &project_name, QWidget *parent,
        std::function<void(const QString &, bool)> on_status)
    : QDialog(parent), target(resolvePersonalRepositoryTarget()), client(this),
      project_id(project_id), branch_id(branch_id), branch_name(branch_name),
      head(head), source_repository_id(source_repository_id),
      source_endpoint(source_endpoint), project_name(project_name),
      on_status(std::move(on_status))
{
    // This is an asynchronous Open Network operation, not a working-copy
    // management dialog. It is displayed only to disambiguate saved drafts.
    setObjectName(QStringLiteral("projectsWorkingCopiesDialog"));
    setWindowTitle(tr("Open hydraulic network in editor"));
    resize(580, 360);
    QVBoxLayout *layout = new QVBoxLayout(this);
    this->context_label = new QLabel(tr("Project: %1\nBranch: %2")
        .arg(this->project_name, this->branch_name), this);
    this->context_label->setWordWrap(true);
    this->context_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(this->context_label);
    this->status = new QLabel(this);
    this->status->setObjectName(QStringLiteral("projectsWorkingCopiesStatus"));
    this->status->setWordWrap(true);
    layout->addWidget(this->status);
    this->drafts = new QListWidget(this);
    this->drafts->setObjectName(QStringLiteral("projectsWorkingCopiesList"));
    layout->addWidget(this->drafts, 1);
    this->details = new QLabel(this);
    this->details->setObjectName(QStringLiteral("projectsWorkingCopyDetails"));
    this->details->setWordWrap(true);
    this->details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(this->details);
    this->scope_label = new QLabel(tr("Opening the project's working network. Supported changes will be "
        "autosaved to a private DVMDB draft. This does not create a commit."), this);
    this->scope_label->setWordWrap(true);
    layout->addWidget(this->scope_label);
    QHBoxLayout *actions = new QHBoxLayout();
    this->refresh_button = new QPushButton(tr("Refresh drafts"), this);
    this->refresh_button->setObjectName(QStringLiteral("projectsWorkingCopiesRefresh"));
    this->reopen_button = new QPushButton(tr("Reopen selected draft"), this);
    this->reopen_button->setObjectName(QStringLiteral("projectsWorkingCopiesReopen"));
    actions->addWidget(this->refresh_button);
    actions->addWidget(this->reopen_button);
    layout->addLayout(actions);
    QHBoxLayout *network_actions = new QHBoxLayout();
    this->view_button = new QPushButton(tr("View in Distribution Monitor"), this);
    this->view_button->setObjectName(QStringLiteral("projectsWorkingCopiesViewNetwork"));
    this->edit_button = new QPushButton(tr("Edit in Distribution Editor"), this);
    this->edit_button->setObjectName(QStringLiteral("projectsWorkingCopiesEditNetwork"));
    network_actions->addWidget(this->view_button);
    network_actions->addWidget(this->edit_button);
    layout->addLayout(network_actions);
    this->close_buttons = new WorkingCopyCloseButtonBox(this);
    if (QPushButton *close_button = this->close_buttons->button(QDialogButtonBox::Close))
        close_button->setText(tr("Close"));
    connect(this->close_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(this->close_buttons);
    // The automatic path never displays a dialog. These controls exist only
    // for the exceptional multiple-draft chooser.
    this->drafts->hide();
    this->details->hide();
    this->refresh_button->hide();
    this->reopen_button->hide();
    this->view_button->hide();
    this->edit_button->hide();
    this->close_buttons->hide();
    connect(this->refresh_button, &QPushButton::clicked, this,
            [this] { refresh(); });
    connect(this->reopen_button, &QPushButton::clicked, this,
            [this] { inspectDraft(); });
    connect(this->view_button, &QPushButton::clicked, this,
            [this] { viewDraftNetwork(); });
    connect(this->edit_button, &QPushButton::clicked, this,
            [this] { editDraftNetwork(); });
    connect(this->drafts, &QListWidget::currentRowChanged, this, [this](int)
    {
        // Changing the selected draft invalidates even a previously verified
        // model while an HTTP response for another draft is in flight.
        this->active_draft.reset();
        this->preview_network.reset();
        this->confirm_before_editing = false;
        this->stale_notice.clear();
        setBusy(this->busy);
    });
    setBusy(false);
}

void PersonalWorkingCopyDialog::start()
{
    if (this->target.kind == PersonalRepositoryTarget::Kind::Unavailable)
        reportFailure(tr("Select a personal database in Projects → Server Inspector first."));
    else
        refresh();
}

void PersonalWorkingCopyDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && this->edit_button != nullptr)
        refreshStaticText();
}

void PersonalWorkingCopyDialog::refreshDraftDetailsText()
{
    if (!this->active_draft.has_value())
        return;
    const PersonalWorkingCopyInfo &draft = *this->active_draft;
    QString text = tr("Draft UUIDv7: %1\nSession: %2\nBase revision: %3\n"
                      "Generation: %4\nSaved changes: %5\n"
                      "Loaded candidate records: %6")
        .arg(draft.id, draft.session_id,
             draft.base_revision.isEmpty() ? tr("Unborn") : draft.base_revision,
             draft.generation, draft.dirty ? tr("Yes") : tr("None"),
             QString::number(draft.changes.size()));
    if (this->preview_network.has_value() && this->preview_draft_id == draft.id
        && this->preview_generation == draft.generation)
    {
        const NetworkHydraulic &network = *this->preview_network;
        text += tr("\n\nVerified hydraulic network: %1\nNetwork UUIDv7: %2\n"
                   "Junctions: %3 · Pipes: %4 · Reservoirs: %5 · Tanks: %6")
            .arg(network.id, network.uuid.toString(QUuid::WithoutBraces),
                 QString::number(network.nodes_junctions.size()),
                 QString::number(network.links_pipes.size()),
                 QString::number(network.nodes_reservoirs.size()),
                 QString::number(network.nodes_tanks.size()));
    }
    this->details->setText(text);
}

void PersonalWorkingCopyDialog::refreshStaticText()
{
    setWindowTitle(tr("Open hydraulic network in editor"));
    this->context_label->setText(tr("Project: %1\nBranch: %2")
        .arg(this->project_name, this->branch_name));
    this->scope_label->setText(tr("Opening the project's working network. Supported changes will be "
        "autosaved to a private DVMDB draft. This does not create a commit."));
    this->refresh_button->setText(tr("Refresh drafts"));
    this->reopen_button->setText(tr("Reopen selected draft"));
    this->view_button->setText(tr("View in Distribution Monitor"));
    this->edit_button->setText(tr("Edit in Distribution Editor"));
    if (QPushButton *close = this->close_buttons->button(QDialogButtonBox::Close))
        close->setText(tr("Close"));
    for (int index = 0; index < this->drafts->count(); ++index)
    {
        QListWidgetItem *item = this->drafts->item(index);
        item->setText(tr("%1 · generation %2%3")
            .arg(item->data(Qt::UserRole).toString(),
                 item->data(Qt::UserRole + 1).toString(),
                 item->data(Qt::UserRole + 2).toBool() ? tr(" · modified") : QString()));
    }
    if (this->active_draft.has_value())
        refreshDraftDetailsText();
    // Retranslate retained progress without restarting an in-flight request.
    if (this->operation_status_key != nullptr)
        refreshOperationStatus();
}

void PersonalWorkingCopyDialog::setOperationStatus(const QString &message)
{
    this->status->setText(message);
    if (this->on_status)
        this->on_status(message, false);
}

void PersonalWorkingCopyDialog::setOperationStatusKey(const char *source_key,
                                                     const QString &argument)
{
    this->operation_status_key = source_key;
    this->operation_status_argument = argument;
    refreshOperationStatus();
}

void PersonalWorkingCopyDialog::refreshOperationStatus()
{
    if (this->operation_status_key == nullptr)
        return;
    QString text = tr(this->operation_status_key);
    if (!this->operation_status_argument.isNull())
        text = text.arg(this->operation_status_argument);
    setOperationStatus(text);
}

void PersonalWorkingCopyDialog::reportFailure(const QString &message)
{
    this->operation_status_key = nullptr;
    this->operation_status_argument.clear();
    this->status->setText(message);
    if (this->on_status) this->on_status(message, true);
    if (this->choosing_draft)
    {
        // A broken or stale draft must not close the only recovery UI;
        // the user can refresh the list or choose a different saved draft.
        this->active_draft.reset();
        this->preview_network.reset();
        setBusy(false);
        return;
    }
    reject();
}

void PersonalWorkingCopyDialog::setBusy(bool busy)
{
    this->busy = busy;
    this->drafts->setEnabled(!busy && this->choosing_draft);
    this->refresh_button->setEnabled(!busy && this->choosing_draft);
    this->reopen_button->setEnabled(!busy && this->choosing_draft
        && this->drafts->currentItem() != nullptr);
    const bool verified_preview = !busy && this->choosing_draft
        && this->active_draft.has_value() && this->preview_network.has_value()
        && this->preview_draft_id == this->active_draft->id
        && this->preview_generation == this->active_draft->generation;
    this->view_button->setEnabled(verified_preview);
    this->edit_button->setEnabled(verified_preview
        && this->target.kind != PersonalRepositoryTarget::Kind::Unavailable);
}

void PersonalWorkingCopyDialog::refresh()
{
    this->active_draft.reset();
    this->preview_network.reset();
    setBusy(true);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Reading personal DVMDB drafts…"));
    this->client.list(this->target, this->project_id, [this](const QList<PersonalWorkingCopyInfo> &items,
                                           const QString &error)
    {
        setBusy(false);
        if (!error.isEmpty()) { reportFailure(error); return; }
        this->active_draft.reset();
        this->preview_network.reset();
        this->preview_draft_id.clear();
        this->preview_generation.clear();
        this->details->clear();
        this->drafts->clear();
        for (const PersonalWorkingCopyInfo &draft : items)
        {
            if (draft.project_id != this->project_id || draft.branch_id != this->branch_id)
                continue;
            QListWidgetItem *item = new QListWidgetItem(
                tr("%1 · generation %2%3").arg(draft.id, draft.generation,
                    draft.dirty ? tr(" · modified") : QString()), this->drafts);
            item->setData(Qt::UserRole, draft.id);
            item->setData(Qt::UserRole + 1, draft.generation);
            item->setData(Qt::UserRole + 2, draft.dirty);
        }
        setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog",
            "%1 saved draft(s) for this branch in your personal DVMDB."),
            QString::number(this->drafts->count()));
        setBusy(false);
        if (this->drafts->count() > 1)
        {
            // Do not guess which saved draft contains the user's edits.
            // Only the conflict-resolution controls are presented; checkout,
            // draft creation and initialization remain automatic operations.
            this->choosing_draft = true;
            this->refresh_button->show();
            this->reopen_button->show();
            this->view_button->show();
            this->edit_button->show();
            this->drafts->show();
            this->details->show();
            this->close_buttons->show();
            if (this->on_status)
                this->on_status(tr("Multiple saved drafts: select the one to open."), false);
            setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog",
                "Multiple working drafts exist for this branch. "
                "Select the draft to view or edit; their changes may differ."));
            // Non-blocking modal recovery keeps the selected project/HEAD
            // stable while the user chooses an existing private draft.
            setBusy(false);
            setWindowModality(Qt::WindowModal);
            if (!isVisible()) open();
            return;
        }
        this->choosing_draft = false;
        if (this->drafts->count() == 1)
        {
            this->drafts->setCurrentRow(0);
            verifyDirectDraft(this->drafts->item(0)->data(Qt::UserRole).toString());
            return;
        }
        if (this->source_repository_id == this->target.repository_id)
        {
            // Project already lives on the designated personal server.
            // No clone, transfer or invented first commit is necessary.
            createDraft(this->head);
            return;
        }
        if (this->source_endpoint.isEmpty())
        {
            reportFailure(tr("This project is not accessible through an HTTP "
                "source for checkout into the selected personal database."));
            return;
        }
        checkoutBranch();
    });
}

void PersonalWorkingCopyDialog::createDraft(const QString &base)
{
    this->active_draft.reset();
    this->preview_network.reset();
    setBusy(true);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Creating durable private draft…"));
    this->client.create(this->target, this->project_id, this->branch_id, base,
                        [this, base](const PersonalWorkingCopyInfo &draft, const QString &error)
    {
        setBusy(false);
        if (!error.isEmpty())
        {
            reportFailure(tr("Draft creation failed: %1. "
                "The project/branch must already exist in your personal DVMDB "
                "with exactly the selected HEAD. If the request timed out, refresh "
                "the draft list before retrying: the server may have completed it.").arg(error));
            return;
        }
        if (draft.project_id != this->project_id || draft.branch_id != this->branch_id
            || draft.base_revision != base)
        {
            reportFailure(tr("Personal DVMDB created a draft for a different project, "
                                     "branch or base. Do not edit it; refresh the repository."));
            return;
        }
        showDraft(draft);
    });
}

void PersonalWorkingCopyDialog::inspectDraft()
{
    // This is the conflict-only "Reopen selected draft" action. Use the same
    // HEAD/generation checks as normal Open Network, never a weaker path.
    if (this->busy || !this->choosing_draft || this->drafts->currentItem() == nullptr)
        return;
    const QString id = this->drafts->currentItem()->data(Qt::UserRole).toString();
    verifyDirectDraft(id);
}

void PersonalWorkingCopyDialog::showDraft(const PersonalWorkingCopyInfo &draft)
{
    this->active_draft = draft;
    this->preview_network.reset();
    this->preview_draft_id.clear();
    this->preview_generation.clear();
    // Update the local list after Create/Initialize; a refresh is not needed
    // to learn a server-acknowledged draft or its new generation.
    QListWidgetItem *found = nullptr;
    for (int index = 0; index < this->drafts->count(); ++index)
    {
        QListWidgetItem *item = this->drafts->item(index);
        if (item->data(Qt::UserRole).toString() == draft.id) { found = item; break; }
    }
    if (found == nullptr)
    {
        found = new QListWidgetItem(this->drafts);
        found->setData(Qt::UserRole, draft.id);
    }
    found->setData(Qt::UserRole + 1, draft.generation);
    found->setData(Qt::UserRole + 2, draft.dirty);
    found->setText(tr("%1 · generation %2%3").arg(draft.id, draft.generation,
        draft.dirty ? tr(" · modified") : QString()));
    {
        const QSignalBlocker block(this->drafts);
        this->drafts->setCurrentItem(found);
    }
    setBusy(false);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Personal draft record loaded successfully."));
    refreshDraftDetailsText();
    // A committed draft is rebased onto its new immutable base revision and
    // its candidateChanges become empty. Discover the entire saved candidate,
    // not just changes, or opening the HEAD after Commit loses its network.
    // This same endpoint handles an unborn draft and a fork from a revision.
    setBusy(true);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Discovering the saved hydraulic network…"));
    this->client.summarizeDraftNetworks(this->target, draft.id,
        [this, draft](const QJsonObject &summary, const QString &summary_error)
    {
        if (!this->active_draft.has_value() || this->active_draft->id != draft.id
            || this->active_draft->generation != draft.generation)
            return; // A newer draft selection superseded this response.
        if (!summary_error.isEmpty())
        {
            setBusy(false);
            reportFailure(tr("Saved draft network discovery failed: %1").arg(summary_error));
            return;
        }
        const QJsonArray networks = summary.value(QStringLiteral("networks")).toArray();
        const QString generation = summary.value(QStringLiteral("generation")).toString();
        if (generation != draft.generation || !summary.value(QStringLiteral("networkCount")).isDouble()
            || summary.value(QStringLiteral("networkCount")).toInt(-1) != networks.size())
        {
            setBusy(false);
            reportFailure(tr("Draft changed during network discovery. Reopen it before editing."));
            return;
        }
        if (networks.isEmpty())
        {
            setBusy(false);
            if (!this->choosing_draft && this->active_draft.has_value()
                && this->active_draft->base_revision.isEmpty()
                && this->active_draft->changes.isEmpty() && !this->active_draft->dirty
                && this->target.kind != PersonalRepositoryTarget::Kind::Unavailable)
                initializeNetwork();
            else
                reportFailure(tr("This saved draft contains no hydraulic network."));
            return;
        }
        if (networks.size() != 1)
        {
            setBusy(false);
            reportFailure(tr("Multiple hydraulic networks exist in this draft. "
                "Choose a network explicitly before editing."));
            return;
        }
        const QJsonObject network_summary = networks.first().toObject();
        const QString network_id = network_summary.value(QStringLiteral("id")).toString();
        if (!DvmdbIdentity::isUuidV7(network_id))
        {
            setBusy(false);
            reportFailure(tr("Saved draft returned an invalid network UUIDv7."));
            return;
        }
        this->client.loadDraftNetwork(this->target, draft.id, network_id,
            [this, draft](const NetworkHydraulic &network,
                          const QString &generation, const QString &error)
        {
            if (!this->active_draft.has_value() || this->active_draft->id != draft.id
                || this->active_draft->generation != draft.generation)
                return;
            if (!error.isEmpty())
            {
                setBusy(false);
                reportFailure(tr("Draft loaded, but hydraulic network hydration failed: %1").arg(error));
                return;
            }
            if (generation != draft.generation)
            {
                setBusy(false);
                reportFailure(tr("Draft changed while loading the hydraulic model. Reopen it to get a consistent view."));
                return;
            }
            this->preview_network = network;
            this->preview_draft_id = draft.id;
            this->preview_generation = generation;
            setBusy(false);
            refreshDraftDetailsText();
            setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Hydraulic model verified and ready for private editing."));
            if (!this->choosing_draft)
                editDraftNetwork();
        });
    });
}

const std::optional<NetworkHydraulic> &PersonalWorkingCopyDialog::selectedPreviewNetwork() const
{
    return this->preview_network;
}

QString PersonalWorkingCopyDialog::selectedPreviewDraftId() const
{
    return this->accepted_draft_id;
}

QString PersonalWorkingCopyDialog::selectedPreviewGeneration() const
{
    return this->accepted_generation;
}

QString PersonalWorkingCopyDialog::selectedEditNetworkId() const
{
    return this->accepted_network_id;
}

bool PersonalWorkingCopyDialog::selectedEditRequested() const
{
    return this->accepted_edit;
}

QString PersonalWorkingCopyDialog::targetRepositoryId() const
{
    return this->target.repository_id;
}

void PersonalWorkingCopyDialog::editDraftNetwork()
{
    if (this->busy || !this->active_draft.has_value()
        || !this->preview_network.has_value()
        || this->preview_draft_id != this->active_draft->id
        || this->preview_generation != this->active_draft->generation
        || this->target.kind == PersonalRepositoryTarget::Kind::Unavailable)
        return;
    if (this->confirm_before_editing)
    {
        // A draft on an older HEAD may contain important unsaved edits.
        // Even the conflict chooser must not bypass this confirmation.
        QMessageBox *warning = new QMessageBox(QMessageBox::Warning,
            tr("Working draft based on older HEAD"), this->stale_notice,
            QMessageBox::NoButton, parentWidget());
        warning->setAttribute(Qt::WA_DeleteOnClose);
        QPushButton *proceed = warning->addButton(tr("Open older draft"), QMessageBox::AcceptRole);
        QPushButton *cancel = warning->addButton(tr("Cancel"), QMessageBox::RejectRole);
        warning->setDefaultButton(cancel);
        const QString draft_id = this->active_draft->id;
        const QString generation = this->active_draft->generation;
        connect(warning, &QMessageBox::finished, this,
            [this, warning, proceed, draft_id, generation](int)
        {
            if (warning->clickedButton() == proceed
                && this->active_draft.has_value()
                && this->active_draft->id == draft_id
                && this->active_draft->generation == generation)
            {
                this->confirm_before_editing = false;
                editDraftNetwork();
            }
            else if (!this->choosing_draft)
                reportFailure(tr("Opening the older draft was cancelled."));
        });
        warning->open();
        return;
    }
    this->accepted_draft_id = this->preview_draft_id;
    this->accepted_generation = this->preview_generation;
    this->accepted_network_id = this->preview_network->uuid.toString(QUuid::WithoutBraces);
    this->accepted_edit = true;
    accept();
}

void PersonalWorkingCopyDialog::viewDraftNetwork()
{
    if (this->busy || !this->active_draft.has_value() || !this->preview_network.has_value()
        || this->active_draft->id != this->preview_draft_id
        || this->active_draft->generation != this->preview_generation)
        return;
    this->accepted_draft_id = this->preview_draft_id;
    this->accepted_generation = this->preview_generation;
    this->accepted_edit = false;
    accept();
}

void PersonalWorkingCopyDialog::initializeNetwork()
{
    if (this->busy || !this->active_draft.has_value()) return;
    const PersonalWorkingCopyInfo draft = *this->active_draft;
    if (!draft.base_revision.isEmpty() || draft.dirty || !draft.changes.isEmpty()
        || !DvmdbIdentity::isUuidV7(draft.session_id)) return;
    const QString name = this->project_name.trimmed();
    if (name.isEmpty())
    {
        reportFailure(tr("The project needs a name before initializing a network."));
        return;
    }
    const QString network_id = DvmdbIdentity::newUuidV7();
    setBusy(true);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Acquiring private draft writer lease…"));
    this->client.acquireLease(this->target, draft.id, draft.session_id, 90,
        [this, draft, network_id, name](const PersonalWorkingCopyLease &lease, const QString &error)
    {
        if (!error.isEmpty())
        {
            setBusy(false);
            reportFailure(tr("Unable to acquire private draft lease: %1").arg(error));
            return;
        }
        if (lease.generation != draft.generation)
        {
            this->client.releaseLease(this->target, draft.id, draft.session_id, lease.token,
                [this](const QString &)
            {
                setBusy(false);
                reportFailure(tr("Draft generation changed. Reopen it before initializing a network."));
            });
            return;
        }
        setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Initializing first hydraulic network in private draft…"));
        this->client.initializeEmptyNetwork(this->target, draft.id, draft.session_id,
            lease.token, lease.generation, network_id, name,
            [this, draft, lease](const PersonalWorkingCopyInfo &updated, const QString &init_error)
        {
            // Release even if the HTTP acknowledgement was lost. Never resend
            // initialization after an uncertain result; reopen to reconcile.
            this->client.releaseLease(this->target, draft.id, draft.session_id, lease.token,
                [this, draft, updated, init_error](const QString &release_error)
            {
                setBusy(false);
                if (!init_error.isEmpty())
                {
                    reportFailure(tr("Network initialization was not confirmed: %1 "
                        "Reopen the draft to check whether it succeeded. %2")
                        .arg(init_error, release_error));
                    return;
                }
                if (updated.project_id != draft.project_id || updated.branch_id != draft.branch_id
                    || !updated.base_revision.isEmpty())
                {
                    reportFailure(tr("Unexpected initialized draft identity. Reopen before proceeding."));
                    return;
                }
                showDraft(updated);
                if (!release_error.isEmpty())
                    reportFailure(tr("Network initialized, but lease release is unconfirmed: %1")
                        .arg(release_error));
            });
        });
    });
}

int PersonalWorkingCopyDialog::queuedLocalEdits(const QString &draft_id) const
{
    return PersonalEditQueue::pendingTransactionFileCount(PersonalEditQueue::defaultRoot(),
        QUuid(this->target.repository_id), QUuid(draft_id));
}

// Opening "the branch" must show the branch's current HEAD. A draft keeps the
// base it was created on, so after a newer commit reached the personal
// repository (another draft's Commit, a later checkout) the single existing
// draft can be older than the branch. An unmodified draft is replaced by one
// at the current HEAD; a modified one is opened only after confirmation,
// because its edits are based on the older revision.
// For a branch from another repository, newer source commits are pulled
// first when the draft is unmodified (a fast-forward bundle import). If that
// fails, e.g. because the personal branch has local commits the source does
// not have, the personal HEAD is used as it is.
void PersonalWorkingCopyDialog::verifyDirectDraft(const QString &draft_id)
{
    if (!DvmdbIdentity::isUuidV7(draft_id)) return;
    this->active_draft.reset();
    this->preview_network.reset();
    this->confirm_before_editing = false;
    setBusy(true);
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Checking the private draft against the branch HEAD…"));
    this->client.reopen(this->target, draft_id,
        [this](const PersonalWorkingCopyInfo &draft, const QString &error)
    {
        if (!error.isEmpty() || draft.project_id != this->project_id ||
            draft.branch_id != this->branch_id)
        {
            setBusy(false);
            reportFailure(error.isEmpty()
                ? tr("Reopened draft belongs to a different project or branch.") : error);
            return;
        }
        this->client.branchHead(this->target, this->project_id, this->branch_id,
            [this, draft](const QString &personal_head, const QString &head_error)
        {
            if (!head_error.isEmpty())
            {
                setBusy(false);
                reportFailure(tr("Cannot read the branch HEAD in your personal DVMDB: %1")
                                      .arg(head_error));
                return;
            }
            const int queued = queuedLocalEdits(draft.id);
            const bool unmodified = !draft.dirty && queued == 0;
            const bool other_repository = this->source_repository_id != this->target.repository_id;
            if (unmodified && other_repository && !this->source_endpoint.isEmpty() &&
                this->head != personal_head && !this->source_pull_attempted)
            {
                this->source_pull_attempted = true;
                setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Updating the branch from its source repository…"));
                this->client.checkoutBranchPrivately(this->target, this->source_endpoint,
                    this->source_repository_id, this->project_id, this->project_name,
                    this->branch_id, this->head, [this, draft](const QString &pull_error)
                {
                    if (!pull_error.isEmpty())
                        this->details->setText(tr("The branch could not be updated from its "
                            "source (%1). Your personal HEAD is used.").arg(pull_error));
                    verifyDirectDraft(draft.id);
                });
                return;
            }
            if (draft.base_revision == personal_head)
            {
                setBusy(false);
                showDraft(draft);
                return;
            }
            if (unmodified)
            {
                replaceStaleDraft(draft, personal_head);
                return;
            }
            const QString notice = tr("This private draft is based on revision %1, but the branch "
                "HEAD is now %2. Its %3 is opened on the older revision; committing it will be "
                "refused while the branch has moved on.")
                .arg(draft.base_revision.isEmpty() ? tr("(no commits)") : draft.base_revision,
                     personal_head.isEmpty() ? tr("(no commits)") : personal_head,
                     queued > 0 ? tr("unsaved queued edits are") : tr("saved edits are"));
            this->confirm_before_editing = true;
            this->stale_notice = notice;
            setBusy(false);
            showDraft(draft);
        });
    });
}

void PersonalWorkingCopyDialog::replaceStaleDraft(const PersonalWorkingCopyInfo &draft,
                                                  const QString &personal_head)
{
    setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Replacing the unmodified draft with one at the current HEAD…"));
    // A fresh session: an editor still holding the draft makes this fail
    // instead of deleting a draft that is in use.
    const QString session_id = DvmdbIdentity::newUuidV7();
    this->client.acquireLease(this->target, draft.id, session_id, 30,
        [this, draft, personal_head, session_id](const PersonalWorkingCopyLease &lease,
                                                  const QString &lease_error)
    {
        if (!lease_error.isEmpty())
        {
            setBusy(false);
            reportFailure(tr("The outdated draft is in use and was not replaced: %1")
                                  .arg(lease_error));
            return;
        }
        if (lease.generation != draft.generation)
        {
            this->client.releaseLease(this->target, draft.id, session_id, lease.token,
                [this](const QString &)
            {
                setBusy(false);
                reportFailure(tr("The draft changed while it was being replaced. Open it again."));
            });
            return;
        }
        this->client.discardDraft(this->target, draft.id, session_id, lease.token,
                                  lease.generation, [this, personal_head](const QString &error)
        {
            if (!error.isEmpty())
            {
                setBusy(false);
                reportFailure(tr("The outdated draft could not be removed: %1").arg(error));
                return;
            }
            createDraft(personal_head);
        });
    });
}

void PersonalWorkingCopyDialog::checkoutBranch()
{
    this->active_draft.reset();
    this->preview_network.reset();
    setBusy(true);
    setOperationStatusKey(this->head.isEmpty()
        ? QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Checking out unborn branch into personal DVMDB…")
        : QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Transferring committed branch into personal DVMDB…"));
    this->client.checkoutBranchPrivately(this->target, this->source_endpoint,
        this->source_repository_id, this->project_id, this->project_name,
        this->branch_id, this->head, [this](const QString &error)
    {
        setBusy(false);
        if (!error.isEmpty())
        {
            reportFailure(tr("Checkout failed: %1").arg(error));
            return;
        }
        setOperationStatusKey(QT_TRANSLATE_NOOP("PersonalWorkingCopyDialog", "Branch checked out to personal DVMDB."));
        createDraft(this->head);
    });
}
