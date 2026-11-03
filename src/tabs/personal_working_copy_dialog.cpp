#include "tabs/personal_working_copy_dialog.h"
#include "projects/dvmdb_identity.h"
#include "projects/personal_edit_queue.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <utility>

PersonalWorkingCopyDialog::PersonalWorkingCopyDialog(
        const QString &project_id, const QString &branch_id,
        const QString &branch_name, const QString &head,
        const QString &source_repository_id, const QString &source_endpoint,
        const QString &project_name, QWidget *parent, bool open_editor_directly,
        std::function<void(const QString &, bool)> on_status)
    : QDialog(parent), target(resolvePersonalRepositoryTarget()), client(this),
      project_id(project_id), branch_id(branch_id), branch_name(branch_name),
      head(head), source_repository_id(source_repository_id),
      source_endpoint(source_endpoint), project_name(project_name),
      direct_edit(open_editor_directly), on_status(std::move(on_status))
{
    setObjectName(QStringLiteral("projectsWorkingCopiesDialog"));
    setWindowTitle(this->direct_edit ? tr("Open hydraulic network in editor")
                                      : tr("Personal working copies"));
    resize(580, 440);
    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *context = new QLabel(this->direct_edit
        ? tr("Project: %1\nBranch: %2").arg(this->project_name, this->branch_name)
        : tr("Project: %1\nBranch: %2\nPinned base: %3")
            .arg(this->project_id, this->branch_name,
                 this->head.isEmpty() ? tr("No commits yet") : this->head), this);
    context->setWordWrap(true);
    context->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(context);
    if (this->target.kind != PersonalRepositoryTarget::Kind::Unavailable
        && this->source_repository_id != this->target.repository_id)
    {
        QLabel *different = new QLabel(tr("Selected database and personal DVMDB are different repositories. "
            "Checkout the selected branch before creating its private draft."), this);
        different->setWordWrap(true);
        layout->addWidget(different);
    }
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
    QLabel *scope = new QLabel(this->direct_edit
        ? tr("Opening the project's working network. Supported changes will be "
             "autosaved to a private DVMDB draft. This does not create a commit.")
        : tr("Checkout and first-network initialization change only your personal DVMDB "
             "draft, never the committed branch. Verified networks can be viewed "
             "in Distribution Monitor. Supported Distribution Editor transactions can "
             "be saved through an explicitly opened fenced private session. "
             "Local Commit and uncaptured model operations are not yet supported."), this);
    scope->setWordWrap(true);
    layout->addWidget(scope);
    QHBoxLayout *actions = new QHBoxLayout();
    this->refresh_button = new QPushButton(tr("Refresh drafts"), this);
    this->refresh_button->setObjectName(QStringLiteral("projectsWorkingCopiesRefresh"));
    this->checkout_button = new QPushButton(tr("Checkout branch privately"), this);
    this->checkout_button->setObjectName(QStringLiteral("projectsWorkingCopiesCheckout"));
    this->create_button = new QPushButton(tr("Create draft at HEAD"), this);
    this->create_button->setObjectName(QStringLiteral("projectsWorkingCopiesCreate"));
    this->reopen_button = new QPushButton(tr("Reopen selected draft"), this);
    this->reopen_button->setObjectName(QStringLiteral("projectsWorkingCopiesReopen"));
    this->initialize_button = new QPushButton(tr("Initialize empty network"), this);
    this->initialize_button->setObjectName(QStringLiteral("projectsWorkingCopiesInitializeNetwork"));
    this->view_button = new QPushButton(tr("View in Distribution Monitor"), this);
    this->view_button->setObjectName(QStringLiteral("projectsWorkingCopiesViewNetwork"));
    this->edit_button = new QPushButton(tr("Edit in Distribution Editor"), this);
    this->edit_button->setObjectName(QStringLiteral("projectsWorkingCopiesEditNetwork"));
    actions->addWidget(this->checkout_button);
    actions->addWidget(this->refresh_button);
    actions->addWidget(this->create_button);
    actions->addWidget(this->reopen_button);
    layout->addLayout(actions);
    QHBoxLayout *network_actions = new QHBoxLayout();
    network_actions->addWidget(this->initialize_button);
    network_actions->addWidget(this->view_button);
    network_actions->addWidget(this->edit_button);
    layout->addLayout(network_actions);
    if (this->direct_edit)
    {
        // Normal project editing is one operation. The advanced working-copy
        // controls remain available through the separate Inspector action.
        this->checkout_button->hide();
        this->refresh_button->hide();
        this->create_button->hide();
        this->reopen_button->hide();
        this->initialize_button->hide();
        this->view_button->hide();
        this->edit_button->hide();
        this->drafts->hide();
    }
    QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
    connect(this->refresh_button, &QPushButton::clicked, this,
            [this] { refresh(); });
    connect(this->checkout_button, &QPushButton::clicked, this,
            [this] { checkoutBranch(); });
    connect(this->create_button, &QPushButton::clicked, this,
            [this] { createDraft(this->head); });
    connect(this->reopen_button, &QPushButton::clicked, this,
            [this] { inspectDraft(); });
    connect(this->initialize_button, &QPushButton::clicked, this,
            [this] { initializeNetwork(); });
    connect(this->view_button, &QPushButton::clicked, this,
            [this] { viewDraftNetwork(); });
    connect(this->edit_button, &QPushButton::clicked, this,
            [this] { editDraftNetwork(); });
    connect(this->drafts, &QListWidget::currentRowChanged, this, [this](int)
    {
        // A previously inspected network must never be opened after switching
        // to another draft, even if the asynchronous response arrives later.
        this->active_draft.reset();
        this->preview_network.reset();
        setBusy(this->busy);
    });
    setBusy(false);
    if (this->target.kind == PersonalRepositoryTarget::Kind::Unavailable)
    {
        reportFailure(tr("Select a personal database in Projects → Server Inspector first."));
        this->create_button->setEnabled(false);
        this->refresh_button->setEnabled(false);
        this->checkout_button->setEnabled(false);
    }
    else
    {
        refresh();
    }
}

void PersonalWorkingCopyDialog::setOperationStatus(const QString &message)
{
    this->status->setText(message);
    if (this->direct_edit && this->on_status)
        this->on_status(message, false);
}

void PersonalWorkingCopyDialog::reportFailure(const QString &message)
{
    this->status->setText(message);
    if (this->direct_edit)
    {
        if (this->on_status) this->on_status(message, true);
        reject();
    }
}

void PersonalWorkingCopyDialog::setBusy(bool busy)
{
    this->busy = busy;
    this->refresh_button->setEnabled(!busy);
    this->checkout_button->setEnabled(!busy &&
        !this->source_endpoint.isEmpty() &&
        this->source_repository_id != this->target.repository_id &&
        this->target.kind != PersonalRepositoryTarget::Kind::Unavailable);
    this->create_button->setEnabled(!busy);
    this->reopen_button->setEnabled(!busy && this->drafts->currentItem() != nullptr);
    this->initialize_button->setEnabled(!busy && this->active_draft.has_value()
        && this->active_draft->base_revision.isEmpty()
        && this->active_draft->changes.isEmpty() && !this->active_draft->dirty
        && this->target.kind != PersonalRepositoryTarget::Kind::Unavailable);
    const bool verified_preview = !busy && this->active_draft.has_value()
        && this->preview_network.has_value()
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
    setOperationStatus(tr("Reading personal DVMDB drafts…"));
    this->client.list(this->target, this->project_id, [this](const QList<PersonalWorkingCopyInfo> &items,
                                           const QString &error)
    {
        setBusy(false);
        if (!error.isEmpty()) { reportFailure(error); return; }
        this->active_draft.reset();
        this->preview_network.reset();
        this->preview_draft_id.clear();
        this->preview_generation.clear();
        this->drafts->clear();
        for (const PersonalWorkingCopyInfo &draft : items)
        {
            if (draft.project_id != this->project_id || draft.branch_id != this->branch_id)
                continue;
            QListWidgetItem *item = new QListWidgetItem(
                tr("%1 · generation %2%3").arg(draft.id, draft.generation,
                    draft.dirty ? tr(" · modified") : QString()), this->drafts);
            item->setData(Qt::UserRole, draft.id);
        }
        setOperationStatus(tr("%1 saved draft(s) for this branch in your personal DVMDB.")
                              .arg(this->drafts->count()));
        if (!this->drafts->count())
            this->details->setText(tr("No private draft for this branch. "
                "Create one only after this project and branch exist in the personal DVMDB."));
        this->reopen_button->setEnabled(false);
        setBusy(false);
        if (!this->direct_edit) return;
        if (this->drafts->count() > 1)
        {
            // Never guess which competing private draft contains the user's
            // edits. The advanced working-copy action permits selecting one.
            reportFailure(tr("Multiple working drafts exist for this branch. "
                "Open Personal working copies and select the draft to edit."));
            return;
        }
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
    setOperationStatus(tr("Creating durable private draft…"));
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
    if (this->drafts->currentItem() == nullptr) return;
    this->active_draft.reset();
    this->preview_network.reset();
    const QString id = this->drafts->currentItem()->data(Qt::UserRole).toString();
    if (!DvmdbIdentity::isUuidV7(id)) return;
    setBusy(true);
    setOperationStatus(tr("Reopening private draft…"));
    this->client.reopen(this->target, id,
                        [this](const PersonalWorkingCopyInfo &draft, const QString &error)
    {
        setBusy(false);
        if (!error.isEmpty()) { reportFailure(error); return; }
        if (draft.project_id != this->project_id || draft.branch_id != this->branch_id)
        {
            reportFailure(tr("Reopened draft belongs to a different project or branch."));
            return;
        }
        showDraft(draft);
    });
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
    found->setText(tr("%1 · generation %2%3").arg(draft.id, draft.generation,
        draft.dirty ? tr(" · modified") : QString()));
    {
        const QSignalBlocker block(this->drafts);
        this->drafts->setCurrentItem(found);
    }
    setBusy(false);
    setOperationStatus(tr("Personal draft record loaded successfully."));
    this->details->setText(tr("Draft UUIDv7: %1\nSession: %2\nBase revision: %3\n"
                              "Generation: %4\nSaved changes: %5\n"
                              "Loaded candidate records: %6")
        .arg(draft.id, draft.session_id,
             draft.base_revision.isEmpty() ? tr("Unborn") : draft.base_revision,
             draft.generation, draft.dirty ? tr("Yes") : tr("None"),
             QString::number(draft.changes.size())));
    // A committed draft is rebased onto its new immutable base revision and
    // its candidateChanges become empty. Discover the entire saved candidate,
    // not just changes, or opening the HEAD after Commit loses its network.
    // This same endpoint handles an unborn draft and a fork from a revision.
    setBusy(true);
    setOperationStatus(tr("Discovering the saved hydraulic network…"));
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
            if (this->direct_edit && this->initialize_button->isEnabled())
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
            this->details->setText(this->details->text() +
                tr("\n\nVerified hydraulic network: %1\nNetwork UUIDv7: %2\n"
                   "Junctions: %3 · Pipes: %4 · Reservoirs: %5 · Tanks: %6")
                .arg(network.id, network.uuid.toString(QUuid::WithoutBraces),
                     QString::number(network.nodes_junctions.size()),
                     QString::number(network.links_pipes.size()),
                     QString::number(network.nodes_reservoirs.size()),
                     QString::number(network.nodes_tanks.size())));
            setOperationStatus(tr("Hydraulic model verified and ready for private editing."));
            if (this->direct_edit && this->confirm_before_editing)
            {
                QMessageBox *warning = new QMessageBox(QMessageBox::Warning,
                    tr("Working draft based on older HEAD"), this->stale_notice,
                    QMessageBox::NoButton, parentWidget());
                warning->setAttribute(Qt::WA_DeleteOnClose);
                QPushButton *proceed = warning->addButton(tr("Open older draft"), QMessageBox::AcceptRole);
                QPushButton *cancel = warning->addButton(tr("Cancel"), QMessageBox::RejectRole);
                warning->setDefaultButton(cancel);
                connect(warning, &QMessageBox::finished, this,
                    [this, warning, proceed](int)
                {
                    if (warning->clickedButton() == proceed) editDraftNetwork();
                    else reportFailure(tr("Opening the older draft was cancelled."));
                });
                warning->open();
            }
            else if (this->direct_edit)
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
    if (!this->edit_button->isEnabled() || !this->preview_network.has_value())
        return;
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
    bool accepted = this->direct_edit;
    const QString name = this->direct_edit ? this->project_name.trimmed()
        : QInputDialog::getText(this, tr("Initialize first hydraulic network"),
            tr("Network name:"), QLineEdit::Normal, tr("Network"), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    const QString network_id = DvmdbIdentity::newUuidV7();
    setBusy(true);
    setOperationStatus(tr("Acquiring private draft writer lease…"));
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
        setOperationStatus(tr("Initializing first hydraulic network in private draft…"));
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
    setOperationStatus(tr("Checking the private draft against the branch HEAD…"));
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
                setOperationStatus(tr("Updating the branch from its source repository…"));
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
    setOperationStatus(tr("Replacing the unmodified draft with one at the current HEAD…"));
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
    setOperationStatus(this->head.isEmpty()
        ? tr("Checking out unborn branch into personal DVMDB…")
        : tr("Transferring committed branch into personal DVMDB…"));
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
        setOperationStatus(tr("Branch checked out to personal DVMDB."));
        if (this->direct_edit)
            createDraft(this->head);
    });
}
