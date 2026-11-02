#pragma once

#include <QHBoxLayout>
#include <QFormLayout>
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
    }

    QLabel *personal_banner = nullptr;
    QPushButton *create_project = nullptr;
};
class ProjectInspectorOverview final : public ProjectsInspectorOverview
{
public:
    explicit ProjectInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Project"), parent)
    {
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        // Project Overview: editable-looking metadata first, branch creation
        // immediately below it, network actions above the identity footer.
        // Keep preview explanations near the fields rather than under the IDs.
        column->removeWidget(this->metadata);
        column->removeWidget(this->preview_note);
        QLayoutItem *old_stretch = column->takeAt(column->count() - 1);
        delete old_stretch;

        this->create_branch = new QPushButton(tr("Create branch"), this);
        this->create_branch->setObjectName(QStringLiteral("projectsInspectorOverviewCreateBranch"));
        this->create_branch->setToolTip(tr("Create a named unborn branch in this project"));
        column->addWidget(this->create_branch, 0, Qt::AlignLeft);
        column->addWidget(this->preview_note);
        column->addStretch(1);

        this->import_network = new QPushButton(tr("Import network…"), this);
        this->import_network->setObjectName(QStringLiteral("projectsInspectorImport"));
        this->import_network->setToolTip(tr("Import a network into the selected project"));
        this->edit_network = new QPushButton(tr("Edit network"), this);
        this->edit_network->setObjectName(QStringLiteral("projectsInspectorEditNetwork"));
        this->edit_network->setToolTip(tr("Open the hydraulic network in Distribution Editor with durable autosave"));
        QHBoxLayout *network_actions = new QHBoxLayout();
        network_actions->addWidget(this->import_network);
        network_actions->addWidget(this->edit_network);
        network_actions->addStretch(1);
        column->addLayout(network_actions);
        column->addWidget(this->metadata);
    }
    QPushButton *create_branch = nullptr;
    QPushButton *import_network = nullptr;
    QPushButton *edit_network = nullptr;
};
class BranchInspectorOverview final : public ProjectsInspectorOverview
{
public:
    explicit BranchInspectorOverview(QWidget *parent = nullptr)
        : ProjectsInspectorOverview(QStringLiteral("Branch"), parent) {}
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
        this->metadata->hide();
    }
    void placePrimaryActions(QPushButton *create_branch, QPushButton *commit_head)
    {
        this->primary_actions->insertWidget(0, create_branch);
        this->primary_actions->insertWidget(1, commit_head);
    }
    QLabel *branch_label = nullptr;
    TwoLineCommitEditor *commit_message = nullptr;
    QLabel *status = nullptr;
    QHBoxLayout *primary_actions = nullptr;
};

// Both HEAD and immutable Revision use this tab to edit the same branch name.
class RevisionInspectorBranch final : public QWidget
{
public:
    explicit RevisionInspectorBranch(QLineEdit *name_edit, QWidget *parent = nullptr)
        : QWidget(parent), name(name_edit)
    {
        setObjectName(QStringLiteral("projectsRevisionBranchTab"));
        QVBoxLayout *column = new QVBoxLayout(this);
        column->setContentsMargins(0, 12, 0, 4);
        QFormLayout *form = new QFormLayout();
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setLabelAlignment(Qt::AlignLeft);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->name->setParent(this);
        form->addRow(tr("Branch name"), this->name);
        this->tags = new QLineEdit(this);
        this->tags->setObjectName(QStringLiteral("projectsRevisionBranchTags"));
        this->tags->setPlaceholderText(tr("hydraulics, approved, scenario"));
        form->addRow(tr("Tags"), this->tags);
        this->description = new QTextEdit(this);
        this->description->setObjectName(QStringLiteral("projectsRevisionBranchDescription"));
        this->description->setMinimumHeight(90);
        this->description->setMaximumHeight(170);
        form->addRow(tr("Description"), this->description);
        column->addLayout(form);
        column->addStretch(1);
    }

    QLineEdit *name = nullptr;
    QLineEdit *tags = nullptr;
    QTextEdit *description = nullptr;
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
public:
    explicit ProjectInspector(QWidget *parent = nullptr) : ProjectsInspectorPanel(parent)
    {
        this->setObjectName(QStringLiteral("projectsProjectInspector"));
        this->delete_project = addButton(tr("Delete project…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Remove this preview project"));
    }
    QPushButton *delete_project = nullptr;

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
        this->working_copies = addButton(tr("Personal working copies…"),
            QStringLiteral("projectsInspectorWorkingCopies"),
            tr("Create or reopen a durable private draft for this exact branch HEAD"));
        this->edit_network = addButton(tr("Edit network"),
            QStringLiteral("projectsInspectorEditNetwork"),
            tr("Open the hydraulic network in Distribution Editor with durable autosave"));
        this->create_branch = addButton(tr("Create branch"),
            QStringLiteral("projectsInspectorCreateBranch"),
            tr("Create a named branch from an explicitly chosen revision"));
        this->delete_branch = addButton(tr("Delete branch…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Remove this preview branch"));
        moveToFullWidth(this->edit_network);
        moveToFullWidth(this->working_copies);
    }
    QPushButton *working_copies = nullptr;
    QPushButton *edit_network = nullptr;
    QPushButton *create_branch = nullptr;
    QPushButton *delete_branch = nullptr;

    void update(bool creating, bool busy, bool ready, bool backed, bool preview, bool valid)
    {
        this->working_copies->setVisible(!creating && backed);
        this->working_copies->setEnabled(!busy);
        this->edit_network->setVisible(!creating && backed);
        this->edit_network->setEnabled(ready);
        this->create_branch->setVisible(!creating);
        this->create_branch->setEnabled(ready);
        this->delete_branch->setVisible(!creating && preview);
        setCreationState(creating, busy, valid);
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
        this->edit_head_network = addButton(tr("Edit network"),
            QStringLiteral("projectsHeadInspectorEditNetwork"),
            tr("Edit this HEAD through a durable private working copy"));
        this->head_working_copies = addButton(tr("Personal working copies…"),
            QStringLiteral("projectsHeadInspectorWorkingCopies"),
            tr("Create or reopen a private working copy of this branch HEAD"));
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
        moveToFullWidth(this->edit_head_network);
        moveToFullWidth(this->head_working_copies);
    }
    QPushButton *create_branch = nullptr;
    QPushButton *edit_head_network = nullptr;
    QPushButton *head_working_copies = nullptr;
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
        this->edit_head_network->setVisible(!creating && head && database_backed);
        this->edit_head_network->setEnabled(ready);
        this->head_working_copies->setVisible(!creating && head && database_backed);
        this->head_working_copies->setEnabled(!busy);
        setCreationState(creating, busy, valid);
    }
};
