#pragma once

#include "tabs/tab_projects_widget.h"
#include "projects/dvmdb_identity.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWidget>

// Connection controls belong exclusively to the database inspector. They are
// never reparented into project/branch/revision forms.
class DatabaseInspectorConnection final : public QWidget
{
public:
    explicit DatabaseInspectorConnection(const QString &default_path, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("projectsInspectorConnection"));
        this->setMinimumWidth(0);
        this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QVBoxLayout *connection_layout = new QVBoxLayout(this);
        connection_layout->setContentsMargins(0, 12, 0, 4);
        this->delete_connection = new QPushButton(tr("Delete connection…"), this);
        this->delete_connection->setObjectName(QStringLiteral("projectsDeleteConnectionButton"));
        this->delete_connection->setToolTip(tr("Remove this connection from AOWIS without deleting the DVMDB repository"));
        this->delete_connection->hide();
        connection_layout->addWidget(this->delete_connection, 0, Qt::AlignRight);
        this->source_name_edit = new QLineEdit(this);
        this->source_name_edit->setObjectName(QStringLiteral("projectsNewSourceName"));
        this->source_name_edit->setPlaceholderText(tr("Database name"));
        connection_layout->addWidget(this->source_name_edit);

        this->server_options = new QWidget(this);
        this->server_options->setObjectName(QStringLiteral("projectServerOptions"));
        QVBoxLayout *source_layout = new QVBoxLayout(this->server_options);
        source_layout->setContentsMargins(0, 0, 0, 0);
        this->source_status = new QLabel(this->server_options);
        this->source_status->setObjectName(QStringLiteral("serversPreviewStatus"));
        wrapLabel(this->source_status);
        source_layout->addWidget(this->source_status);
        this->source_refresh_button = new QPushButton(tr("Refresh catalogue"), this->server_options);
        this->source_refresh_button->setToolTip(tr("Reload this database now; automatic loading and health checks continue in the background"));
        this->source_refresh_button->setMinimumHeight(28);
        this->source_refresh_button->setObjectName(QStringLiteral("projectsSourceRefreshButton"));
        source_layout->addWidget(this->source_refresh_button);
        this->personal_server_checkbox = new QCheckBox(tr("Use as my personal working database"), this->server_options);
        this->personal_server_checkbox->setObjectName(QStringLiteral("projectsPersonalServerCheckbox"));
        this->personal_server_checkbox->setToolTip(tr("Only one database can be personal. Its identity is discovered automatically; no UUID input is needed."));
        source_layout->addWidget(this->personal_server_checkbox);

        this->source_kind_combo = new QComboBox(this->server_options);
        this->source_kind_combo->setObjectName(QStringLiteral("projectSourceKind"));
        this->source_kind_combo->addItem(tr("Remote database (REST)"), QStringLiteral("remote"));
        this->source_kind_combo->addItem(tr("Local database (embedded)"), QStringLiteral("local"));
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
        if (QStandardItemModel *model = qobject_cast<QStandardItemModel *>(this->source_kind_combo->model()))
            model->item(1)->setEnabled(false);
#endif
        QFormLayout *kind_form = new QFormLayout();
        kind_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        kind_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        kind_form->addRow(tr("Connection type"), this->source_kind_combo);
        source_layout->addLayout(kind_form);

        this->remote_options = new QWidget(this->server_options);
        QFormLayout *server_form = new QFormLayout(this->remote_options);
        server_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        server_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->server_endpoint_edit = new QLineEdit(this->remote_options);
        this->server_endpoint_edit->setObjectName(QStringLiteral("projectServerEndpoint"));
        this->server_endpoint_edit->setPlaceholderText(tr("https://database.example.invalid"));
        this->server_auth_combo = new QComboBox(this->remote_options);
        this->server_auth_combo->setObjectName(QStringLiteral("projectServerAuthMode"));
        this->server_auth_combo->addItem(tr("None"), QStringLiteral("None"));
        this->server_auth_combo->addItem(tr("API token"), QStringLiteral("API token"));
        this->server_auth_combo->addItem(tr("User login"), QStringLiteral("User login"));
        server_form->addRow(tr("REST endpoint"), this->server_endpoint_edit);
        server_form->addRow(tr("Authentication (future)"), this->server_auth_combo);
        QLabel *server_warning = new QLabel(tr("Automatically monitored and loaded. REST authentication is not yet supported; credentials and tokens are not stored."), this->remote_options);
        wrapLabel(server_warning);
        server_form->addRow(server_warning);
        source_layout->addWidget(this->remote_options);

        this->local_options = new QWidget(this->server_options);
        QFormLayout *local_form = new QFormLayout(this->local_options);
        local_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        local_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->local_path_edit = new QLineEdit(this->local_options);
        this->local_path_edit->setObjectName(QStringLiteral("projectLocalDvmdbPath"));
        this->local_path_edit->setPlaceholderText(default_path);
        this->local_path_edit->setMinimumWidth(0);
        this->browse_path = new QPushButton(tr("Browse…"), this->local_options);
        this->browse_path->setToolTip(tr("Choose a local DVMDB data directory"));
        this->browse_path->setMinimumHeight(28);
        this->browse_path->setObjectName(QStringLiteral("projectLocalBrowseButton"));
        QHBoxLayout *local_path_row = new QHBoxLayout();
        local_path_row->addWidget(this->local_path_edit, 1);
        local_path_row->addWidget(this->browse_path);
        local_form->addRow(tr("Data directory"), local_path_row);
        QLabel *local_warning = new QLabel(tr("Local DVMDB uses the embedded C++ library. Choose a dedicated data directory, not a directory used by a running HTTP server."), this->local_options);
        wrapLabel(local_warning);
        local_form->addRow(local_warning);
        source_layout->addWidget(this->local_options);
        connection_layout->addWidget(this->server_options);
        connection_layout->addStretch(1);
    }

    void updateMode(bool visible, bool creating, bool busy)
    {
        this->server_options->setVisible(visible);
        if (!visible) return;
        this->source_kind_combo->setEnabled(creating);
        const bool local = this->source_kind_combo->currentData().toString() == QStringLiteral("local");
        this->remote_options->setVisible(!local);
        this->local_options->setVisible(local);
        this->personal_server_checkbox->setVisible(!creating);
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        this->personal_server_checkbox->setEnabled(!busy);
#else
        // The ordinary GUI can display local profiles, but only Standalone
        // embeds DVMDB. Do not offer a designation that only opens a warning.
        this->personal_server_checkbox->setEnabled(!busy && !local);
        this->personal_server_checkbox->setToolTip(local
            ? tr("Selecting an embedded personal database requires AOWIS Standalone.")
            : tr("Only one database can be personal. Its identity is discovered automatically; no UUID input is needed."));
#endif
    }

    QLineEdit *source_name_edit = nullptr;
    QWidget *server_options = nullptr;
    QLabel *source_status = nullptr;
    QPushButton *source_refresh_button = nullptr;
    QCheckBox *personal_server_checkbox = nullptr;
    QComboBox *source_kind_combo = nullptr;
    QWidget *remote_options = nullptr;
    QLineEdit *server_endpoint_edit = nullptr;
    QComboBox *server_auth_combo = nullptr;
    QWidget *local_options = nullptr;
    QLineEdit *local_path_edit = nullptr;
    QPushButton *browse_path = nullptr;
    QPushButton *delete_connection = nullptr;

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

// Each object's related page owns its list and the data-to-row mapping.
// ProjectsWidget handles selection routing, never rebuilding another domain's list.
class ProjectsInspectorRelations : public QWidget
{
public:
    explicit ProjectsInspectorRelations(const QString &name, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(name);
        this->setMinimumWidth(0);
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 12, 0, 4);
        this->list = new QListWidget(this);
        this->list->setObjectName(name + QStringLiteral("List"));
        layout->addWidget(this->list);
    }
    QListWidget *list = nullptr;

protected:
    void addRelation(const QString &text, const QString &key)
    {
        QListWidgetItem *item = new QListWidgetItem(text, this->list);
        item->setData(Qt::UserRole, key);
    }
};

class DatabaseInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit DatabaseInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsDatabaseRelations"), parent)
    {
        this->create_project = new QPushButton(tr("Create project"), this);
        this->create_project->setObjectName(QStringLiteral("projectsDatabaseProjectsCreateProject"));
        this->create_project->setToolTip(tr("Create a project in this database"));
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        column->insertWidget(0, this->create_project, 0, Qt::AlignLeft);
    }
    QPushButton *create_project = nullptr;

    void populate(const QList<ProjectPreviewData> &projects, const QString &server_id)
    {
        this->list->clear();
        for (const ProjectPreviewData &project : projects)
            if (project.server_id == server_id)
                addRelation(project.name, QStringLiteral("p:") + project.id);
    }
};

class ProjectInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit ProjectInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsProjectRelations"), parent) {}
    void populate(const ProjectPreviewData *project)
    {
        this->list->clear();
        if (project == nullptr) return;
        for (const ProjectPreviewBranch &branch : project->branches)
            addRelation(branch.head.isEmpty()
                    ? tr("%1  ·  No commits yet").arg(branch.name)
                    : tr("%1  ·  HEAD %2").arg(branch.name, DvmdbIdentity::shortLabel(branch.head)),
                QStringLiteral("b:") + branch.id);
    }
};

class BranchInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit BranchInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsBranchRelations"), parent) {}
    void populate(const ProjectPreviewData *project, const ProjectPreviewBranch *branch)
    {
        this->list->clear();
        if (project == nullptr || branch == nullptr) return;
        if (branch->head.isEmpty())
        {
            QListWidgetItem *item = new QListWidgetItem(tr("No commits yet"), this->list);
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
        }
        QSet<QString> reachable;
        QStringList remaining{branch->head};
        QHash<QString, QStringList> parents;
        for (const ProjectPreviewRevision &revision : project->revisions)
            parents.insert(revision.id, revision.parents);
        while (!remaining.isEmpty())
        {
            const QString id = remaining.takeLast();
            if (id.isEmpty() || reachable.contains(id)) continue;
            reachable.insert(id);
            for (const QString &parent : parents.value(id))
                remaining.append(parent);
        }
        for (const ProjectPreviewRevision &revision : project->revisions)
            if (reachable.contains(revision.id))
                addRelation(revision.id == branch->head
                        ? tr("%1  ·  HEAD").arg(DvmdbIdentity::shortLabel(revision.id))
                        : DvmdbIdentity::shortLabel(revision.id),
                    QStringLiteral("r:") + revision.id);
    }
};

class RevisionInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit RevisionInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsRevisionRelations"), parent) {}
    void populate(const ProjectPreviewData *project, const ProjectPreviewRevision *revision)
    {
        this->list->clear();
        if (revision == nullptr) return;
        for (const QString &parent : revision->parents)
            addRelation(tr("Parent · %1").arg(DvmdbIdentity::shortLabel(parent)),
                        QStringLiteral("r:") + parent);
        if (project == nullptr) return;
        for (const ProjectPreviewBranch &branch : project->branches)
            if (branch.head == revision->id)
                addRelation(tr("%1 · HEAD").arg(branch.name), QStringLiteral("b:") + branch.id);
    }
};
