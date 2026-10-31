#pragma once

#include <QHBoxLayout>
#include <QPushButton>
#include <QString>
#include <QSize>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

// Each domain owns its action surface. ProjectsWidget routes selection and
// performs operations; no action button is shared or reparented between kinds.
// A later split of metadata forms can use these same stable widget boundaries.
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
        this->create_project = addButton(tr("Create project"),
            QStringLiteral("projectsInspectorCreateProject"), tr("Create a project in this database"));
        this->delete_database = addButton(tr("Delete database…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Remove this database profile from the GUI configuration"));
        this->cancel_source = addButton(tr("Cancel"),
            QStringLiteral("projectSourceCancelButton"), tr("Discard this unsaved database"));
        this->save_source = addButton(tr("Create"),
            QStringLiteral("projectSourceCreateButton"), tr("Save the new database profile"));
    }
    QPushButton *create_project = nullptr;
    QPushButton *delete_database = nullptr;
    QPushButton *save_source = nullptr;
    QPushButton *cancel_source = nullptr;

    void update(bool new_source, bool creating, bool busy, bool ready, bool deletable, bool valid)
    {
        this->create_project->setVisible(!new_source && !creating);
        this->create_project->setEnabled(ready);
        this->delete_database->setVisible(!new_source && !creating && deletable);
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
        this->import_network = addButton(tr("Import network…"),
            QStringLiteral("projectsInspectorImport"), tr("Import a network into the selected project"));
        this->edit_network = addButton(tr("Edit network"),
            QStringLiteral("projectsInspectorEditNetwork"),
            tr("Open the hydraulic network in Distribution Editor with durable autosave"));
        this->create_branch = addButton(tr("Create branch"),
            QStringLiteral("projectsInspectorCreateBranch"),
            tr("Create a named branch from an explicitly chosen revision"));
        this->delete_project = addButton(tr("Delete project…"),
            QStringLiteral("projectInspectorDeleteButton"), tr("Remove this preview project"));
        moveToFullWidth(this->edit_network);
        moveToFullWidth(this->import_network);
    }
    QPushButton *import_network = nullptr;
    QPushButton *edit_network = nullptr;
    QPushButton *create_branch = nullptr;
    QPushButton *delete_project = nullptr;

    void update(bool creating, bool busy, bool ready, bool backed, bool preview, bool valid)
    {
        this->import_network->setVisible(!creating);
        this->import_network->setEnabled(!busy);
        this->edit_network->setVisible(!creating && backed);
        this->edit_network->setEnabled(ready);
        this->create_branch->setVisible(!creating);
        this->create_branch->setEnabled(ready);
        this->delete_project->setVisible(!creating && preview);
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
            tr("Create a named branch from this immutable revision"));
    }
    QPushButton *create_branch = nullptr;
    void update(bool creating, bool busy, bool ready, bool valid)
    {
        this->create_branch->setVisible(!creating);
        this->create_branch->setEnabled(ready);
        setCreationState(creating, busy, valid);
    }
};
