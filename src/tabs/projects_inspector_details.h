#pragma once

#include "config/live_ui_text.h"
#include "widgets/group_box_collapsible.h"

#include "tabs/tab_projects_widget.h"
#include "projects/dvmdb_identity.h"

#include <QCoreApplication>
#include <QCheckBox>
#include <QSignalBlocker>
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
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit DatabaseInspectorConnection(const QString &default_path, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("projectsInspectorConnection"));
        this->setMinimumWidth(0);
        this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QVBoxLayout *connection_layout = new QVBoxLayout(this);
        connection_layout->setContentsMargins(0, 12, 0, 4);
        this->source_name_edit = new QLineEdit(this);
        this->source_name_edit->setObjectName(QStringLiteral("projectsNewSourceName"));
        LiveUiText::bind(this->source_name_edit, [this] { this->source_name_edit->setPlaceholderText(tr("Database name")); });
        connection_layout->addWidget(this->source_name_edit);

        this->server_options = new QWidget(this);
        this->server_options->setObjectName(QStringLiteral("projectServerOptions"));
        QVBoxLayout *source_layout = new QVBoxLayout(this->server_options);
        source_layout->setContentsMargins(0, 0, 0, 0);
        this->source_status = new QLabel(this->server_options);
        this->source_status->setObjectName(QStringLiteral("serversPreviewStatus"));
        wrapLabel(this->source_status);
        source_layout->addWidget(this->source_status);
        this->source_refresh_button = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Refresh catalogue"), this->server_options);
        LiveUiText::bind(this->source_refresh_button, [this] { this->source_refresh_button->setToolTip(tr("Reload this database now; automatic loading and health checks continue in the background")); });
        this->source_refresh_button->setMinimumHeight(28);
        this->source_refresh_button->setObjectName(QStringLiteral("projectsSourceRefreshButton"));
        source_layout->addWidget(this->source_refresh_button);
        this->personal_server_checkbox = LiveUiText::caption<QCheckBox>(QT_TRANSLATE_NOOP("LiveUiText", "Use as my personal working database"), this->server_options);
        this->personal_server_checkbox->setObjectName(QStringLiteral("projectsPersonalServerCheckbox"));
        
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
        kind_form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Connection type"), this), this->source_kind_combo);
        source_layout->addLayout(kind_form);

        this->remote_options = new QWidget(this->server_options);
        QFormLayout *server_form = new QFormLayout(this->remote_options);
        server_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        server_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->server_endpoint_edit = new QLineEdit(this->remote_options);
        this->server_endpoint_edit->setObjectName(QStringLiteral("projectServerEndpoint"));
        this->server_endpoint_edit->setPlaceholderText(QStringLiteral("https://database.example.invalid"));
        this->server_auth_combo = new QComboBox(this->remote_options);
        this->server_auth_combo->setObjectName(QStringLiteral("projectServerAuthMode"));
        this->server_auth_combo->addItem(tr("None"), QStringLiteral("None"));
        this->server_auth_combo->addItem(tr("API token"), QStringLiteral("API token"));
        this->server_auth_combo->addItem(tr("User login"), QStringLiteral("User login"));
        server_form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "REST endpoint"), this), this->server_endpoint_edit);
        server_form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Authentication (future)"), this), this->server_auth_combo);
        QLabel *server_warning = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Automatically monitored and loaded. REST authentication is not yet supported; credentials and tokens are not stored."), this->remote_options);
        wrapLabel(server_warning);
        server_form->addRow(server_warning);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        GroupBoxCollapsible *wireguard = new GroupBoxCollapsible(QStringLiteral("WireGuard"), this->remote_options);
        wireguard->setObjectName(QStringLiteral("projectWireGuardGroup"));
        QVBoxLayout *profile_layout = new QVBoxLayout(wireguard);
        this->wireguard_path = new QLineEdit(wireguard);
        this->wireguard_path->setObjectName(QStringLiteral("projectWireGuardProfile"));
        this->wireguard_path->setReadOnly(true);
        LiveUiText::bind(this->wireguard_path, [this] {
            this->wireguard_path->setPlaceholderText(tr("Direct connection"));
        });
        profile_layout->addWidget(this->wireguard_path);
        this->choose_wireguard = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Choose WireGuard profile…"), wireguard);
        this->clear_wireguard = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Use direct connection"), wireguard);
        this->verify_wireguard = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Verify repository"), wireguard);
        profile_layout->addWidget(this->choose_wireguard);
        profile_layout->addWidget(this->clear_wireguard);
        this->verify_wireguard->setObjectName(QStringLiteral("projectsVerifyRepository"));
        profile_layout->addWidget(this->verify_wireguard);
        QLabel *scope = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "WireGuard is used for verification, health checks and catalogue refresh. Opening and editing projects still use the direct connection."), wireguard);
        wrapLabel(scope);
        profile_layout->addWidget(scope);
        wireguard->setCollapsed(true);
        server_form->addRow(wireguard);
#endif
        source_layout->addWidget(this->remote_options);

        this->local_options = new QWidget(this->server_options);
        QFormLayout *local_form = new QFormLayout(this->local_options);
        local_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        local_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        this->local_path_edit = new QLineEdit(this->local_options);
        this->local_path_edit->setObjectName(QStringLiteral("projectLocalDvmdbPath"));
        this->local_path_edit->setPlaceholderText(default_path);
        this->local_path_edit->setMinimumWidth(0);
        this->browse_path = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Browse…"), this->local_options);
        LiveUiText::bind(this->browse_path, [this] { this->browse_path->setToolTip(tr("Choose a local DVMDB data directory")); });
        this->browse_path->setMinimumHeight(28);
        this->browse_path->setObjectName(QStringLiteral("projectLocalBrowseButton"));
        QHBoxLayout *local_path_row = new QHBoxLayout();
        local_path_row->addWidget(this->local_path_edit, 1);
        local_path_row->addWidget(this->browse_path);
        local_form->addRow(LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Data directory"), this), local_path_row);
        QLabel *local_warning = LiveUiText::caption<QLabel>(QT_TRANSLATE_NOOP("LiveUiText", "Local DVMDB uses the embedded C++ library. Choose a dedicated data directory, not a directory used by a running HTTP server."), this->local_options);
        wrapLabel(local_warning);
        local_form->addRow(local_warning);
        source_layout->addWidget(this->local_options);
        connection_layout->addWidget(this->server_options);
        connection_layout->addStretch(1);
        this->delete_connection = LiveUiText::caption<QPushButton>(QT_TRANSLATE_NOOP("LiveUiText", "Delete connection"), this);
        this->delete_connection->setObjectName(QStringLiteral("projectsDeleteConnectionButton"));
        LiveUiText::bind(this->delete_connection, [this] { this->delete_connection->setToolTip(tr("Remove this connection from AOWIS without deleting the DVMDB repository")); });
        LiveUiText::bind(this, [this] {
            const QSignalBlocker kind_blocker(this->source_kind_combo);
            const QSignalBlocker auth_blocker(this->server_auth_combo);
            this->source_kind_combo->setItemText(0, tr("Remote database (REST)"));
            this->source_kind_combo->setItemText(1, tr("Local database (embedded)"));
            this->server_auth_combo->setItemText(0, tr("None"));
            this->server_auth_combo->setItemText(1, tr("API token"));
            this->server_auth_combo->setItemText(2, tr("User login"));
            refreshPersonalToolTip();
        });
        this->delete_connection->hide();
        connection_layout->addWidget(this->delete_connection, 0, Qt::AlignLeft);
    }

    void updateMode(bool visible, bool creating, bool busy)
    {
        this->server_options->setVisible(visible);
        if (!visible) return;
        this->source_kind_combo->setEnabled(creating);
        const bool local = this->source_kind_combo->currentData().toString() == QStringLiteral("local");
        this->remote_options->setVisible(!local);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        this->choose_wireguard->setEnabled(!busy);
        this->clear_wireguard->setEnabled(!busy && !this->wireguard_path->text().isEmpty());
        this->verify_wireguard->setEnabled(!busy && !creating);
#endif
        this->local_options->setVisible(local);
        this->personal_server_checkbox->setVisible(!creating);
#ifdef AOWIS_STANDALONE_DVMDB_ENABLED
        this->personal_server_checkbox->setEnabled(!busy);
#else
        // The ordinary GUI can display local profiles, but only Standalone
        // embeds DVMDB. Do not offer a designation that only opens a warning.
        this->personal_server_checkbox->setEnabled(!busy && !local);
        refreshPersonalToolTip();
#endif
    }

    QLineEdit *source_name_edit = nullptr;
    QWidget *server_options = nullptr;
    QLabel *source_status = nullptr;
    QPushButton *source_refresh_button = nullptr;
    QCheckBox *personal_server_checkbox = nullptr;
    QComboBox *source_kind_combo = nullptr;
    QWidget *remote_options = nullptr;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    QLineEdit *wireguard_path = nullptr;
    QPushButton *choose_wireguard = nullptr;
    QPushButton *clear_wireguard = nullptr;
    QPushButton *verify_wireguard = nullptr;
#endif
    QLineEdit *server_endpoint_edit = nullptr;
    QComboBox *server_auth_combo = nullptr;
    QWidget *local_options = nullptr;
    QLineEdit *local_path_edit = nullptr;
    QPushButton *browse_path = nullptr;
    QPushButton *delete_connection = nullptr;

private:
    void refreshPersonalToolTip()
    {
#ifndef AOWIS_STANDALONE_DVMDB_ENABLED
        if (this->source_kind_combo->currentData().toString() == QStringLiteral("local"))
        {
            this->personal_server_checkbox->setToolTip(tr("Selecting an embedded personal database requires AOWIS Standalone."));
            return;
        }
#endif
        this->personal_server_checkbox->setToolTip(tr("Only one database can be personal. Its identity is discovered automatically; no UUID input is needed."));
    }

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
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
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
    QListWidgetItem *addRelation(const QString &text, const QString &key)
    {
        QListWidgetItem *item = new QListWidgetItem(text, this->list);
        item->setData(Qt::UserRole, key);
        return item;
    }
};

class DatabaseInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit DatabaseInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsDatabaseRelations"), parent)
    {
        this->create_project = new QPushButton(this);
        this->create_project->setObjectName(QStringLiteral("projectsDatabaseProjectsCreateProject"));
        LiveUiText::bind(this->create_project, [this] {
            this->create_project->setText(tr("Create project"));
            this->create_project->setToolTip(tr("Create a project in this database"));
        });
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        column->addWidget(this->create_project, 0, Qt::AlignLeft);
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
    Q_DECLARE_TR_FUNCTIONS(ProjectsWidget)
public:
    explicit ProjectInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsProjectRelations"), parent)
    {
        this->create_branch = new QPushButton(this);
        this->create_branch->setObjectName(QStringLiteral("projectsInspectorCreateBranch"));
        refreshLanguage();
        QVBoxLayout *column = qobject_cast<QVBoxLayout *>(this->layout());
        column->addWidget(this->create_branch, 0, Qt::AlignLeft);
    }
    QPushButton *create_branch = nullptr;

    void populate(const ProjectPreviewData *project)
    {
        this->list->clear();
        if (project == nullptr) return;
        for (const ProjectPreviewBranch &branch : project->branches)
        {
            QListWidgetItem *item = new QListWidgetItem(this->list);
            item->setData(Qt::UserRole, QStringLiteral("b:") + branch.id);
            item->setData(Qt::UserRole + 1, branch.name);
            item->setData(Qt::UserRole + 2, branch.head);
            translateRow(item);
        }
    }

    void refreshLanguage()
    {
        this->create_branch->setText(tr("Create branch"));
        this->create_branch->setToolTip(tr("Create a named unborn branch in this project"));
        for (int row = 0; row < this->list->count(); ++row)
            translateRow(this->list->item(row));
    }

private:
    static void translateRow(QListWidgetItem *item)
    {
        const QString name = item->data(Qt::UserRole + 1).toString();
        const QString head = item->data(Qt::UserRole + 2).toString();
        item->setText(head.isEmpty() ? tr("%1  ·  No commits yet").arg(name)
                                    : tr("%1  ·  HEAD %2").arg(name, DvmdbIdentity::shortLabel(head)));
    }
};

class BranchInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit BranchInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsBranchRelations"), parent) {}
    void refreshLanguage()
    {
        for (int row = 0; row < this->list->count(); ++row)
        {
            QListWidgetItem *item = this->list->item(row);
            if (!item->data(Qt::UserRole).isValid())
                item->setText(tr("No commits yet"));
            else if (item->data(Qt::UserRole + 1).isValid())
                item->setText(tr("%1  ·  HEAD").arg(item->data(Qt::UserRole + 1).toString()));
        }
    }

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
            {
                const QString label = DvmdbIdentity::shortLabel(revision.id);
                QListWidgetItem *item = addRelation(label, QStringLiteral("r:") + revision.id);
                if (revision.id == branch->head)
                    item->setData(Qt::UserRole + 1, label);
            }
        refreshLanguage();
    }
};

class RevisionInspectorRelations final : public ProjectsInspectorRelations
{
public:
    explicit RevisionInspectorRelations(QWidget *parent = nullptr)
        : ProjectsInspectorRelations(QStringLiteral("projectsRevisionRelations"), parent) {}
    void refreshLanguage()
    {
        for (int row = 0; row < this->list->count(); ++row)
        {
            QListWidgetItem *item = this->list->item(row);
            const bool parent = item->data(Qt::UserRole).toString().startsWith(QStringLiteral("r:"));
            const QString label = item->data(Qt::UserRole + 1).toString();
            item->setText(parent ? tr("Parent · %1").arg(label) : tr("%1 · HEAD").arg(label));
        }
    }

    void populate(const ProjectPreviewData *project, const ProjectPreviewRevision *revision)
    {
        this->list->clear();
        if (revision == nullptr) return;
        for (const QString &parent : revision->parents)
            addRelation(QString(), QStringLiteral("r:") + parent)
                ->setData(Qt::UserRole + 1, DvmdbIdentity::shortLabel(parent));
        if (project != nullptr)
            for (const ProjectPreviewBranch &branch : project->branches)
                if (branch.head == revision->id)
                    addRelation(QString(), QStringLiteral("b:") + branch.id)
                        ->setData(Qt::UserRole + 1, branch.name);
        refreshLanguage();
    }
};
