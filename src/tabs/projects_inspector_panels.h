#pragma once

#include "config/live_ui_text.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QFont>
#include <QtMath>
#include <QEvent>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QSize>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

// Each domain owns its form and action surface. The HEAD/Revision primary
// buttons are displayed in that domain's Overview directly after Description.
// No controls are shared across different inspector domains.
class ProjectsInspectorOverview : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit ProjectsInspectorOverview(const QString &kind, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("projects") + kind + QStringLiteral("OverviewForm"));
        this->setMinimumWidth(0);
        this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QVBoxLayout *column = new QVBoxLayout(this);
        column->setContentsMargins(0, 0, 0, 0);
        this->fields = new QWidget(this);
        QFormLayout *form = new QFormLayout(this->fields);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setLabelAlignment(Qt::AlignLeft);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->name = new QLineEdit(this->fields);
        this->name->setObjectName(QStringLiteral("projectInspectorName"));
        this->tags = new QLineEdit(this->fields);
        this->tags->setObjectName(QStringLiteral("projectInspectorTags"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        this->description = new QTextEdit(this->fields);
        this->description->setObjectName(QStringLiteral("projectInspectorDescription"));
        this->description->setMinimumHeight(90);
        this->description->setMaximumHeight(170);
        form->addRow(kind == QStringLiteral("Project") ? tr("Name")
                     : kind == QStringLiteral("Revision") ? tr("Branch name")
                     : tr("Name / title"), this->name);
        form->addRow(tr("Tags"), this->tags);
        form->addRow(tr("Description"), this->description);
        column->addWidget(this->fields);
        this->metadata = new QLabel(this);
        this->metadata->setObjectName(QStringLiteral("projectInspectorMetadata"));
        wrapLabel(this->metadata);
        this->metadata->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(this->metadata);
        this->preview_note = new QLabel(this);
        this->preview_note->setObjectName(QStringLiteral("inspectorPreviewNote"));
        wrapLabel(this->preview_note);
        column->addWidget(this->preview_note);
        column->addStretch(1);
    }
    // A form owns its content and presentation state. The selection coordinator
    // passes domain values, rather than repointing shared member aliases.
    void present(const QString &title, const QStringList &tag_list,
                 const QString &details, const QString &info,
                 bool editable, bool show_fields)
    {
        const QSignalBlocker name_blocker(this->name);
        const QSignalBlocker tags_blocker(this->tags);
        const QSignalBlocker description_blocker(this->description);
        this->name->setText(title);
        this->tags->setText(tag_list.join(QStringLiteral(", ")));
        this->description->setPlainText(details);
        this->metadata->setText(info);
        this->name->setEnabled(editable);
        this->tags->setEnabled(editable);
        this->description->setEnabled(editable);
        this->fields->setVisible(show_fields);
    }

    void setPreviewExplanation(const QString &explanation)
    {
        this->preview_note->setText(explanation);
    }

    QWidget *fields = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *tags = nullptr;
    QTextEdit *description = nullptr;
    QLabel *metadata = nullptr;
    QLabel *preview_note = nullptr;
private:
    static void wrapLabel(QLabel *label)
    {
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        label->setSizePolicy(policy);
    }
};

class DatabaseInspectorOverview final : public ProjectsInspectorOverview
{
public:
    explicit DatabaseInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Database"), parent)
    {
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        // Only the Database Overview has this order: personal designation,
        // name and other editable fields, Create project, then repository ID.
        QLayoutItem *old_stretch = column->takeAt(column->count() - 1);
        delete old_stretch;
        this->personal_banner = new QLabel(tr("♛ Personal working database"), this);
        this->personal_banner->setObjectName(QStringLiteral("projectsPersonalDatabaseBanner"));
        QFont banner_font = this->personal_banner->font();
        banner_font.setBold(true);
        this->personal_banner->setFont(banner_font);
        this->personal_banner->hide();
        column->insertWidget(0, this->personal_banner);

        this->create_project = new QPushButton(tr("Create project"), this);
        this->create_project->setObjectName(QStringLiteral("projectsInspectorCreateProject"));
        this->create_project->setToolTip(tr("Create a project in this database"));
        column->insertWidget(column->indexOf(this->metadata),
                             this->create_project, 0, Qt::AlignLeft);
        column->insertStretch(column->indexOf(this->metadata), 1);
        LiveUiText::bind(this, [this] {
            QFormLayout *form = static_cast<QFormLayout *>(this->fields->layout());
            static_cast<QLabel *>(form->labelForField(this->name))->setText(tr("Name / title"));
            static_cast<QLabel *>(form->labelForField(this->tags))->setText(tr("Tags"));
            static_cast<QLabel *>(form->labelForField(this->description))->setText(tr("Description"));
            this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
            this->personal_banner->setText(tr("♛ Personal working database"));
            this->create_project->setText(tr("Create project"));
            this->create_project->setToolTip(tr("Create a project in this database"));
        });
    }

    QLabel *personal_banner = nullptr;
    QPushButton *create_project = nullptr;
};
class ProjectInspectorOverview final : public ProjectsInspectorOverview
{
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit ProjectInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Project"), parent)
    {
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        // Project Overview: metadata first, branch creation and import above
        // the identity footer. Opening a network belongs to the selected HEAD.
        // Keep preview explanations near the fields rather than under the IDs.
        column->removeWidget(this->metadata);
        column->removeWidget(this->preview_note);
        QLayoutItem *old_stretch = column->takeAt(column->count() - 1);
        delete old_stretch;

        this->create_branch = new QPushButton(this);
        this->create_branch->setObjectName(QStringLiteral("projectsInspectorOverviewCreateBranch"));
        column->addWidget(this->create_branch, 0, Qt::AlignLeft);
        column->addWidget(this->preview_note);
        column->addStretch(1);

        this->import_network = new QPushButton(this);
        this->import_network->setObjectName(QStringLiteral("projectsInspectorImport"));
        column->addWidget(this->import_network, 0, Qt::AlignLeft);
        column->addWidget(this->metadata);
        refreshLanguage();
    }
    QPushButton *create_branch = nullptr;
    QPushButton *import_network = nullptr;
protected:
    void changeEvent(QEvent *event) override
    {
        ProjectsInspectorOverview::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    void refreshLanguage()
    {
        QFormLayout *form = static_cast<QFormLayout *>(this->fields->layout());
        static_cast<QLabel *>(form->labelForField(this->name))->setText(tr("Name"));
        static_cast<QLabel *>(form->labelForField(this->tags))->setText(tr("Tags"));
        static_cast<QLabel *>(form->labelForField(this->description))->setText(tr("Description"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        this->create_branch->setText(tr("Create branch"));
        this->create_branch->setToolTip(tr("Create a named unborn branch in this project"));
        this->import_network->setText(tr("Import network…"));
        this->import_network->setToolTip(tr("Import a network into the selected project"));
    }
};
class BranchInspectorOverview final : public ProjectsInspectorOverview
{
public:
    explicit BranchInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Branch"), parent) {}
protected:
    void changeEvent(QEvent *event) override
    {
        ProjectsInspectorOverview::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    void refreshLanguage()
    {
        QFormLayout *form = static_cast<QFormLayout *>(this->fields->layout());
        static_cast<QLabel *>(form->labelForField(this->name))->setText(tr("Name / title"));
        static_cast<QLabel *>(form->labelForField(this->tags))->setText(tr("Tags"));
        static_cast<QLabel *>(form->labelForField(this->description))->setText(tr("Description"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
    }
};
// Compact plain-text commit editor. Its viewport shows exactly two font lines;
// more content scrolls, and a font/style change recalculates the physical height.
class TwoLineCommitEditor final : public QPlainTextEdit
{
public:
    explicit TwoLineCommitEditor(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setTabChangesFocus(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        updateTwoLineHeight();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange
            || event->type() == QEvent::StyleChange
            || event->type() == QEvent::ApplicationFontChange)
            updateTwoLineHeight();
    }

private:
    void updateTwoLineHeight()
    {
        const int line_height = fontMetrics().lineSpacing();
        const int document_margin = qCeil(document()->documentMargin());
        const QMargins margins = contentsMargins();
        setFixedHeight(2 * line_height + 2 * document_margin
                             + 2 * frameWidth() + margins.top() + margins.bottom());
    }
};

class RevisionInspectorOverview final : public ProjectsInspectorOverview
{
public:
    explicit RevisionInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Revision"), parent)
    {
        QFormLayout *form = qobject_cast<QFormLayout *>(this->fields->layout());
        // The branch editor belongs to the Branch tab, not Overview. Keep the
        // existing QLineEdit instance so presentation and autosave use one state.
        const QFormLayout::TakeRowResult name_row = form->takeRow(0);
        if (name_row.labelItem != nullptr) {
            delete name_row.labelItem->widget();
            delete name_row.labelItem;
        }
        delete name_row.fieldItem; // does not delete the detached QLineEdit
        this->branch_label = new QLabel(this->fields);
        this->branch_label->setObjectName(QStringLiteral("projectsRevisionBranchLabel"));
        this->branch_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->insertRow(0, tr("Branch"), this->branch_label);
        this->commit_message = new TwoLineCommitEditor(this->fields);
        this->commit_message->setObjectName(QStringLiteral("projectsRevisionCommitMessage"));
        form->insertRow(2, tr("Commit message"), this->commit_message);
        this->status = new QLabel(this);
        this->status->setObjectName(QStringLiteral("projectsRevisionStatus"));
        this->status->setWordWrap(true);
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        column->insertWidget(column->indexOf(this->preview_note), this->status);
        this->primary_actions = new QHBoxLayout();
        this->primary_actions->setSpacing(5);
        this->primary_actions->addStretch(1);
        column->insertLayout(column->indexOf(this->status), this->primary_actions);
        this->commit_explanation_title = new QLabel(tr("Why commit a revision?"), this);
        this->commit_explanation_title->setObjectName(
            QStringLiteral("projectsHeadCommitExplanationTitle"));
        QFont explanation_font = this->commit_explanation_title->font();
        explanation_font.setBold(true);
        this->commit_explanation_title->setFont(explanation_font);
        column->insertWidget(column->indexOf(this->status), this->commit_explanation_title);
        this->commit_explanation = new QLabel(this);
        this->commit_explanation->setObjectName(
            QStringLiteral("projectsHeadCommitExplanation"));
        this->commit_explanation->setWordWrap(true);
        this->commit_explanation->setMinimumWidth(0);
        QSizePolicy explanation_policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        explanation_policy.setHeightForWidth(true);
        this->commit_explanation->setSizePolicy(explanation_policy);
        column->insertWidget(column->indexOf(this->status), this->commit_explanation);
        this->commit_explanation_title->hide();
        this->commit_explanation->hide();
        this->metadata->hide();
        refreshLanguage();
    }
    void setCommitExplanationVisible(bool visible)
    {
        this->commit_explanation_title->setVisible(visible);
        this->commit_explanation->setVisible(visible);
    }
    void placePrimaryActions(QPushButton *create_branch, QPushButton *commit_head)
    {
        this->primary_actions->insertWidget(0, create_branch);
        this->primary_actions->insertWidget(1, commit_head);
    }
    QLabel *branch_label = nullptr;
    TwoLineCommitEditor *commit_message = nullptr;
    QLabel *status = nullptr;
    QLabel *commit_explanation_title = nullptr;
    QLabel *commit_explanation = nullptr;
    QHBoxLayout *primary_actions = nullptr;
protected:
    void changeEvent(QEvent *event) override
    {
        ProjectsInspectorOverview::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    void refreshLanguage()
    {
        QFormLayout *form = static_cast<QFormLayout *>(this->fields->layout());
        static_cast<QLabel *>(form->labelForField(this->branch_label))->setText(tr("Branch"));
        static_cast<QLabel *>(form->labelForField(this->commit_message))->setText(tr("Commit message"));
        static_cast<QLabel *>(form->labelForField(this->tags))->setText(tr("Tags"));
        static_cast<QLabel *>(form->labelForField(this->description))->setText(tr("Description"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        this->commit_explanation_title->setText(tr("Why commit a revision?"));
        this->commit_explanation->setText(tr("Committing creates an immutable snapshot of your hydraulic network, "
               "preserving its exact state at that point in time.\n\n"
               "This allows you to:\n"
               "\u2022 Return to an earlier version of your network.\n"
               "\u2022 Compare changes between revisions.\n"
               "\u2022 Create new branches to explore alternatives without modifying the original.\n\n"
               "Each commit requires a short message describing your changes.\n\n"
               "You can continue editing afterward. Your HEAD remains writable, and "
               "changes are automatically saved to your private working copy."));
    }
};

// The selected revision/HEAD's network entry point. This is deliberately not
// a duplicate editor for commit metadata: HEAD/Revision and Branch have their
// own tabs and fields. Counts are supplied by a revision-pinned DVMDB read.
class RevisionNetworkOverview final : public ProjectsInspectorOverview
{
public:
    struct Message
    {
        const char *source = nullptr;
        QStringList arguments;

        QString translated() const
        {
            if (this->source == nullptr)
                return {};
            const QString format = QCoreApplication::translate("ProjectsWidget", this->source);
            if (this->arguments.size() == 2)
                return format.arg(this->arguments.first(), this->arguments.last());
            if (this->arguments.size() == 1)
                return format.arg(this->arguments.first());
            return format;
        }
    };

    explicit RevisionNetworkOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Network"), parent)
    {
        this->setObjectName(QStringLiteral("projectsRevisionNetworkOverview"));
        this->fields->hide();
        this->metadata->hide();
        this->preview_note->hide();
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        QLayoutItem *stretch = column->takeAt(column->count() - 1);
        delete stretch;
        this->branch = new QLabel(this);
        this->branch->setObjectName(QStringLiteral("projectsNetworkOverviewBranch"));
        this->branch->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->insertWidget(0, this->branch);
        this->state = new QLabel(this);
        this->state->setObjectName(QStringLiteral("projectsNetworkOverviewState"));
        column->insertWidget(1, this->state);
        this->heading = new QLabel(this);
        QFont heading_font = this->heading->font();
        heading_font.setBold(true);
        this->heading->setFont(heading_font);
        column->addWidget(this->heading);
        this->network_status = new QLabel(this);
        this->network_status->setTextFormat(Qt::PlainText);
        this->network_status->setObjectName(QStringLiteral("projectsNetworkOverviewSummaryStatus"));
        this->network_status->setWordWrap(true);
        this->network_status->setMinimumWidth(0);
        column->addWidget(this->network_status);
        const QStringList captions = countCaptions();
        QWidget *counts_widget = new QWidget(this);
        QGridLayout *counts_layout = new QGridLayout(counts_widget);
        counts_layout->setContentsMargins(0, 0, 0, 0);
        counts_layout->setHorizontalSpacing(8);
        counts_layout->setVerticalSpacing(8);
        for (int index = 0; index < captions.size(); ++index) {
            QLabel *value = new QLabel(QStringLiteral("—"), counts_widget);
            value->setObjectName(QStringLiteral("projectsNetworkCount") + QString::number(index));
            QFont font = value->font();
            font.setBold(true);
            value->setFont(font);
            QLabel *caption = new QLabel(captions.at(index), counts_widget);
            QVBoxLayout *item_layout = new QVBoxLayout();
            item_layout->setContentsMargins(2, 2, 2, 2);
            item_layout->addWidget(caption);
            item_layout->addWidget(value);
            counts_layout->addLayout(item_layout, index / 2, index % 2);
            this->counts.append(value);
            this->count_labels.append(caption);
        }
        column->addWidget(counts_widget);
        this->other_counts = new QLabel(this);
        this->other_counts->setObjectName(QStringLiteral("projectsNetworkOverviewOtherCounts"));
        this->other_counts->setWordWrap(true);
        column->addWidget(this->other_counts);
        this->open_network = new QPushButton(tr("Open network"), this);
        this->open_network->setObjectName(QStringLiteral("projectsNetworkOverviewOpen"));
        column->addWidget(this->open_network);
        QHBoxLayout *actions = new QHBoxLayout();
        this->import_network = new QPushButton(tr("Import network…"), this);
        this->import_network->setObjectName(QStringLiteral("projectsNetworkOverviewImport"));
        this->export_network = new QPushButton(tr("Export network…"), this);
        this->export_network->setObjectName(QStringLiteral("projectsNetworkOverviewExport"));
        actions->addWidget(this->import_network);
        actions->addWidget(this->export_network);
        column->addLayout(actions);
        this->delete_revision = new QPushButton(tr("Delete revision and later history…"), this);
        this->delete_revision->setObjectName(QStringLiteral("projectsRevisionOverviewDelete"));
        column->addWidget(this->delete_revision);
        column->addStretch(1);
        clearCounts();
        refreshLanguage();
    }

    void clearCounts()
    {
        for (QLabel *value : this->counts) value->setText(QStringLiteral("—"));
        this->total_count = -1;
        this->other_counts->clear();
    }

    void showCounts(const QJsonObject &counts, int total)
    {
        const QStringList keys{QStringLiteral("junctions"), QStringLiteral("pipes"),
            QStringLiteral("reservoirs"), QStringLiteral("tanks"),
            QStringLiteral("pumps"), QStringLiteral("valves")};
        for (int index = 0; index < keys.size(); ++index)
            this->counts.at(index)->setText(QLocale().toString(counts.value(keys.at(index)).toInt()));
        this->total_count = total;
        refreshTotal();
    }

    void setStatus(Message message, Message detail)
    {
        this->status_message = message;
        this->status_detail = detail;
        refreshStatus();
    }

    void setStatus(Message message)
    {
        setStatus(message, {});
    }

    QLabel *branch = nullptr;
    QLabel *state = nullptr;
    QLabel *network_status = nullptr;
    QList<QLabel *> counts;
    QLabel *other_counts = nullptr;
    QPushButton *open_network = nullptr;
    QPushButton *delete_revision = nullptr;
    QPushButton *import_network = nullptr;
    QPushButton *export_network = nullptr;

protected:
    void changeEvent(QEvent *event) override
    {
        ProjectsInspectorOverview::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    static QStringList countCaptions()
    {
        return {tr("Junctions"), tr("Pipes"), tr("Reservoirs"),
                tr("Tanks"), tr("Pumps"), tr("Valves")};
    }

    void refreshStatus()
    {
        this->network_status->setText(this->status_message.translated() + this->status_detail.translated());
    }

    void refreshTotal()
    {
        if (this->total_count >= 0)
            this->other_counts->setText(tr("Total hydraulic entities: %1").arg(QLocale().toString(this->total_count)));
    }

    void refreshLanguage()
    {
        this->heading->setText(tr("Hydraulic network"));
        const QStringList captions = countCaptions();
        for (int index = 0; index < captions.size(); ++index)
            this->count_labels.at(index)->setText(captions.at(index));
        this->open_network->setText(tr("Open network"));
        this->import_network->setText(tr("Import network…"));
        this->export_network->setText(tr("Export network…"));
        refreshStatus();
        refreshTotal();
    }

    QLabel *heading = nullptr;
    QList<QLabel *> count_labels;
    int total_count = -1;
    Message status_message{QT_TRANSLATE_NOOP("ProjectsWidget", "Checking network…"), {}};
    Message status_detail;
};

// Both HEAD and immutable Revision use this tab to edit the same branch name.
class RevisionInspectorBranch final : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit RevisionInspectorBranch(QLineEdit *name_edit, QWidget *parent = nullptr)
        : QWidget(parent), name(name_edit)
    {
        setObjectName(QStringLiteral("projectsRevisionBranchTab"));
        QVBoxLayout *column = new QVBoxLayout(this);
        column->setContentsMargins(0, 12, 0, 4);
        this->form = new QFormLayout();
        this->form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        this->form->setLabelAlignment(Qt::AlignLeft);
        this->form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->name->setParent(this);
        this->form->addRow(tr("Branch name"), this->name);
        this->tags = new QLineEdit(this);
        this->tags->setObjectName(QStringLiteral("projectsRevisionBranchTags"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        this->form->addRow(tr("Tags"), this->tags);
        this->description = new QTextEdit(this);
        this->description->setObjectName(QStringLiteral("projectsRevisionBranchDescription"));
        this->description->setMinimumHeight(90);
        this->description->setMaximumHeight(170);
        this->form->addRow(tr("Description"), this->description);
        column->addLayout(this->form);
        this->delete_branch = new QPushButton(tr("Delete entire branch…"), this);
        this->delete_branch->setObjectName(QStringLiteral("projectsRevisionBranchDelete"));
        column->addWidget(this->delete_branch);
        column->addStretch(1);
    }

    QPushButton *delete_branch = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *tags = nullptr;
    QTextEdit *description = nullptr;
protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    QFormLayout *form = nullptr;

    void refreshLanguage()
    {
        static_cast<QLabel *>(this->form->labelForField(this->name))->setText(tr("Branch name"));
        static_cast<QLabel *>(this->form->labelForField(this->tags))->setText(tr("Tags"));
        static_cast<QLabel *>(this->form->labelForField(this->description))->setText(tr("Description"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        this->delete_branch->setText(tr("Delete entire branch…"));
    }
};

class ProjectsInspectorActionStack final : public QStackedWidget
{
public:
    explicit ProjectsInspectorActionStack(QWidget *parent = nullptr) : QStackedWidget(parent) {}
    QSize sizeHint() const override
    {
        const QWidget *page = currentWidget();
        return page != nullptr ? page->sizeHint() : QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override
    {
        const QWidget *page = currentWidget();
        return page != nullptr ? page->minimumSizeHint() : QStackedWidget::minimumSizeHint();
    }
    void showPanel(QWidget *panel)
    {
        setCurrentWidget(panel);
        updateGeometry();
    }
};

class ProjectsInspectorPanel : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit ProjectsInspectorPanel(QWidget *parent = nullptr) : QWidget(parent)
    {
        this->setMinimumWidth(0);
        this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
        this->column = new QVBoxLayout(this);
        this->column->setContentsMargins(0, 0, 0, 0);
        this->column->setSpacing(5);
        this->buttons = new QHBoxLayout();
        this->buttons->setSpacing(5);
        this->buttons->addStretch(1);
        this->column->addLayout(this->buttons);
        this->creation_cancel = addButton(tr("Cancel"), QStringLiteral("projectsCreationCancel"),
            tr("Discard the creation form"));
        this->creation_confirm = addButton(tr("Create"), QStringLiteral("projectsCreationConfirm"),
            tr("Create the selected object"));
        LiveUiText::bind(this, [this] {
            this->creation_cancel->setText(tr("Cancel"));
            this->creation_confirm->setText(tr("Create"));
            this->creation_cancel->setToolTip(tr("Discard the creation form"));
            this->creation_confirm->setToolTip(tr("Create the selected object"));
        });
        this->creation_cancel->hide();
        this->creation_confirm->hide();
    }

    QPushButton *creation_cancel = nullptr;
    QPushButton *creation_confirm = nullptr;

    void setCreationState(bool creating, bool busy, bool valid)
    {
        this->creation_cancel->setVisible(creating);
        this->creation_cancel->setEnabled(!busy);
        this->creation_confirm->setVisible(creating);
        this->creation_confirm->setEnabled(creating && !busy && valid);
    }

protected:
    QPushButton *addButton(const QString &text, const QString &object_name,
                           const QString &tooltip)
    {
        QPushButton *button = new QPushButton(text, this);
        button->setObjectName(object_name);
        button->setToolTip(tooltip);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        this->buttons->insertWidget(this->buttons->count() - 2, button);
        return button;
    }

    void moveToFullWidth(QPushButton *button)
    {
        this->buttons->removeWidget(button);
        this->column->insertWidget(0, button);
    }

    QVBoxLayout *column = nullptr;
    QHBoxLayout *buttons = nullptr;
};

class DatabaseInspector final : public ProjectsInspectorPanel
{
public:
    explicit DatabaseInspector(QWidget *parent = nullptr) : ProjectsInspectorPanel(parent)
    {
        this->setObjectName(QStringLiteral("projectsDatabaseInspector"));
        this->cancel_source = addButton(tr("Cancel"),
            QStringLiteral("projectSourceCancelButton"), tr("Discard this unsaved database"));
        this->save_source = addButton(tr("Create"),
            QStringLiteral("projectSourceCreateButton"), tr("Save the new database profile"));
        LiveUiText::bind(this, [this] {
            this->save_source->setText(tr("Create"));
            this->save_source->setToolTip(tr("Save the new database profile"));
            this->cancel_source->setText(tr("Cancel"));
            this->cancel_source->setToolTip(tr("Discard this unsaved database"));
        });
    }
    QPushButton *save_source = nullptr;
    QPushButton *cancel_source = nullptr;

    void update(bool new_source, bool creating, bool busy, bool valid)
    {
        this->save_source->setVisible(new_source);
        this->cancel_source->setVisible(new_source);
        setCreationState(creating, busy, valid);
    }
};

class ProjectInspector final : public ProjectsInspectorPanel
{
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit ProjectInspector(QWidget *parent = nullptr) : ProjectsInspectorPanel(parent)
    {
        this->setObjectName(QStringLiteral("projectsProjectInspector"));
        this->delete_project = addButton(tr("Delete project…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Remove this preview project"));
    }
    QPushButton *delete_project = nullptr;

    void refreshLanguage()
    {
        this->delete_project->setText(tr("Delete project…"));
        this->delete_project->setToolTip(tr("Remove this preview project"));
    }

    void update(bool creating, bool busy, bool preview, bool valid)
    {
        this->delete_project->setVisible(!creating && preview);
        this->delete_project->setEnabled(!busy);
        setCreationState(creating, busy, valid);
    }
};

class BranchInspector final : public ProjectsInspectorPanel
{
public:
    explicit BranchInspector(QWidget *parent = nullptr) : ProjectsInspectorPanel(parent)
    {
        this->setObjectName(QStringLiteral("projectsBranchInspector"));
        this->create_branch = addButton(tr("Create branch"),
            QStringLiteral("projectsInspectorCreateBranch"),
            tr("Create a named branch from an explicitly chosen revision"));
        this->delete_branch = addButton(tr("Delete branch…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Delete the selected branch and its unshared branch history"));
    }
    QPushButton *create_branch = nullptr;
    QPushButton *delete_branch = nullptr;

    void update(bool creating, bool busy, bool ready, bool backed, bool preview, bool valid)
    {
        this->create_branch->setVisible(!creating);
        this->create_branch->setEnabled(ready);
        this->delete_branch->setVisible(!creating && (preview || backed));
        this->delete_branch->setEnabled(!busy && (preview || backed));
        setCreationState(creating, busy, valid);
    }
protected:
    void changeEvent(QEvent *event) override
    {
        ProjectsInspectorPanel::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            refreshLanguage();
    }

private:
    void refreshLanguage()
    {
        this->create_branch->setText(tr("Create branch"));
        this->create_branch->setToolTip(tr("Create a named branch from an explicitly chosen revision"));
        this->delete_branch->setText(tr("Delete branch…"));
        this->delete_branch->setToolTip(tr("Delete the selected branch and its unshared branch history"));
    }
};

class RevisionInspector final : public ProjectsInspectorPanel
{
public:
    explicit RevisionInspector(QWidget *parent = nullptr) : ProjectsInspectorPanel(parent)
    {
        this->setObjectName(QStringLiteral("projectsRevisionInspector"));
        this->create_branch = addButton(tr("Create branch"),
            QStringLiteral("projectsInspectorCreateBranch"),
            tr("Create a named branch from this selected revision or HEAD"));
        this->create_branch->setAttribute(Qt::WA_AlwaysShowToolTips);
        this->commit_head = addButton(tr("Commit"),
            QStringLiteral("projectsHeadInspectorCommit"),
            tr("Commit the saved private working copy with the entered message"));
        this->identity_footer = new QLabel(this);
        this->identity_footer->setObjectName(QStringLiteral("projectsRevisionIdentityFooter"));
        this->identity_footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
        this->identity_footer->setWordWrap(true);
        this->identity_footer->setMinimumWidth(0);
        this->identity_footer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        this->column->addWidget(this->identity_footer);
    }
    QPushButton *create_branch = nullptr;
    QPushButton *commit_head = nullptr;
    QLabel *identity_footer = nullptr;
    void placePrimaryActions(RevisionInspectorOverview *overview)
    {
        this->buttons->removeWidget(this->create_branch);
        this->buttons->removeWidget(this->commit_head);
        this->create_branch->setParent(overview);
        this->commit_head->setParent(overview);
        overview->placePrimaryActions(this->create_branch, this->commit_head);
    }
    void update(bool creating, bool busy, bool ready, bool valid,
                bool head, bool database_backed, bool has_branch, bool has_commit_message)
    {
        this->create_branch->setVisible(!creating);
        this->create_branch->setEnabled(ready && !head);
        this->create_branch->setToolTip(head
            ? tr("Commit HEAD first, then select the finalized revision to create a branch.")
            : tr("Create a new branch from this immutable revision"));
        this->commit_head->setVisible(!creating && head);
        this->commit_head->setEnabled(ready && database_backed && has_branch && has_commit_message);
        // HEAD's details tab explains committing instead of repeating UUIDs.
        // Immutable revision identities remain visible in the Revision inspector.
        this->identity_footer->setVisible(!creating && !head);
        setCreationState(creating, busy, valid);
    }
};
