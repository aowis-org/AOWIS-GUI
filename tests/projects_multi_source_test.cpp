#include "projects/model_codec_compatibility.h"
#include "tabs/tab_projects_widget.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"
#include "services/http_health_monitor.h"
#include "widgets/group_box_collapsible.h"
#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "config/wireguard_profile_storage.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QTcpServer>
#include <QPushButton>
#include <QProcess>
#include <QMessageBox>
#include <QTemporaryFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QRadioButton>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cmath>
#include <functional>
#include <memory>

namespace
{
bool require(bool value, const char *description)
{
    if (!value) std::fprintf(stderr, "projects_multi_source: %s\n", description);
    return value;
}

bool waitUntil(QApplication &app, const std::function<bool()> &condition, int timeout_ms = 5000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout_ms)
        app.processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

QMessageBox *visibleMessage(QWidget &parent)
{
    for (QMessageBox *message : parent.findChildren<QMessageBox *>())
        if (message->isVisible()) return message;
    return nullptr;
}

struct FakeDvmdb
{
    QTcpServer server;
    bool valid = true;
    bool graph_valid = true;
    bool catalogue_valid = true;
    int catalogue_requests = 0;
    int tracking_requests = 0;
    int role_posts = 0;
    QString role = QStringLiteral("unconfigured");
    QJsonObject created_project;
    bool include_unborn_branch = false;
    bool include_second_project = false;
    bool project_deleted = false;        // POST .../delete was accepted
    QString deleted_expected_name;
    const QString unborn_branch_id = QStringLiteral("018f84c0-0000-7000-8000-00000000c002");
    const QString second_project_id = QStringLiteral("018f84c0-0000-7000-8000-00000000a002");
    const QString second_branch_id = QStringLiteral("018f84c0-0000-7000-8000-00000000c003");
    QString repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d001");
    QString hash = QStringLiteral("018f84c0-0000-7000-8000-00000000b001");
    const QString project_id = QStringLiteral("018f84c0-0000-7000-8000-00000000a001");
    const QString branch_id = QStringLiteral("018f84c0-0000-7000-8000-00000000c001");
    const QString digest = QString(64, QLatin1Char('a'));

    bool start()
    {
        if (!this->server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&this->server, &QTcpServer::newConnection, &this->server, [this]
        {
            while (this->server.hasPendingConnections())
            {
                QTcpSocket *socket = this->server.nextPendingConnection();
                const std::shared_ptr<QByteArray> buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffer]
                {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n")) return;
                    const QByteArray path = buffer->split(' ').value(1);
                    QByteArray status = "200 OK";
                    QByteArray data = R"({"broken":true})";
                    if (this->valid)
                    {
                        if (path == "/dvmdb/v1/repository")
                            data = QJsonDocument(QJsonObject{
                                {QStringLiteral("repositoryId"), this->repository_id},
                                {QStringLiteral("modelCodecFingerprint"),
                                 ModelCodecCompatibility::fingerprint()}}).toJson();
                        else if (path == "/dvmdb/v1/repository/tracking")
                        {
                            ++this->tracking_requests;
                            data = QJsonDocument(QJsonObject{
                                {QStringLiteral("role"), this->role},
                                {QStringLiteral("repositoryId"), this->repository_id},
                                {QStringLiteral("modelCodecFingerprint"), ModelCodecCompatibility::fingerprint()}
                            }).toJson();
                        }
                        else if (path == QStringLiteral("/dvmdb/v1/projects/%1/delete").arg(this->project_id).toUtf8()
                                 && buffer->startsWith("POST "))
                        {
                            const qsizetype header_end = buffer->indexOf("\r\n\r\n");
                            qint64 length = -1;
                            for (const QByteArray &header : buffer->left(header_end).split('\n'))
                                if (header.left(header.indexOf(':')).toLower() == "content-length")
                                    length = header.mid(header.indexOf(':') + 1).trimmed().toLongLong();
                            const QByteArray body = buffer->mid(header_end + 4);
                            if (body.size() < length) return;
                            this->deleted_expected_name = QJsonDocument::fromJson(body).object()
                                .value(QStringLiteral("expectedName")).toString();
                            this->project_deleted = true;
                            data = QJsonDocument(QJsonObject{{QStringLiteral("deleted"), true},
                                {QStringLiteral("id"), this->project_id}}).toJson();
                        }
                        else if ((path == "/dvmdb/v1/projects" || path == "/dvmdb/v1/repository/role")
                                 && buffer->startsWith("POST "))
                        {
                            const qsizetype header_end = buffer->indexOf("\r\n\r\n");
                            const QList<QByteArray> headers = buffer->left(header_end).split('\n');
                            qint64 length = -1;
                            QByteArray expected_id;
                            for (const QByteArray &header : headers)
                            {
                                const QByteArray name = header.left(header.indexOf(':')).toLower();
                                const QByteArray value = header.mid(header.indexOf(':') + 1).trimmed();
                                if (name == "content-length") length = value.toLongLong();
                                if (name == "x-aowis-expected-repository-id") expected_id = value;
                            }
                            const QByteArray body = buffer->mid(header_end + 4);
                            if (body.size() < length) return;
                            const QJsonObject json = QJsonDocument::fromJson(body).object();
                            if (path == "/dvmdb/v1/repository/role")
                            {
                                ++this->role_posts;
                                if (length != body.size() || this->role != QStringLiteral("unconfigured")
                                    || json.value(QStringLiteral("expectedRepositoryId")).toString() != this->repository_id
                                    || json.value(QStringLiteral("role")).toString() != QStringLiteral("private"))
                                    status = "400 Bad Request";
                                else
                                {
                                    this->role = QStringLiteral("private");
                                    data = QJsonDocument(QJsonObject{{QStringLiteral("repositoryId"), this->repository_id},
                                        {QStringLiteral("role"), this->role}}).toJson();
                                }
                            }
                            else if (length != body.size() || expected_id != this->repository_id.toUtf8()
                                || !DvmdbIdentity::isUuidV7(json.value(QStringLiteral("id")).toString()))
                                status = "400 Bad Request";
                            else
                            {
                                this->created_project = json;
                                data = QJsonDocument(json).toJson();
                                status = "201 Created";
                            }
                        }
                        else if (path == "/dvmdb/v1/projects")
                        {
                            ++this->catalogue_requests;
                            if (this->catalogue_valid)
                            {
                                QJsonArray projects;
                                if (!this->project_deleted)
                                    projects.append(QJsonObject{
                                        {QStringLiteral("id"), this->project_id},
                                        {QStringLiteral("name"), QStringLiteral("Shared name")}});
                                if (this->include_second_project)
                                    projects.append(QJsonObject{{QStringLiteral("id"), this->second_project_id},
                                        {QStringLiteral("name"), QStringLiteral("Other personal project")}});
                                if (!this->created_project.isEmpty()) projects.append(this->created_project);
                                data = QJsonDocument(QJsonObject{{QStringLiteral("projects"), projects}}).toJson();
                            }
                        }
                        else if (path == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256")
                                     .arg(this->project_id).toUtf8())
                        {
                            if (!this->graph_valid)
                                data = R"({"broken":true})";
                            else
                            {
                                const QJsonObject revision{
                                    {QStringLiteral("id"), this->hash},
                                    {QStringLiteral("recordHash"), this->digest},
                                    {QStringLiteral("parents"), QJsonArray{}},
                                    {QStringLiteral("parentRecordHashes"), QJsonArray{}}};
                                QJsonArray heads{QJsonObject{
                                    {QStringLiteral("branch"), QStringLiteral("main")},
                                    {QStringLiteral("branchId"), this->branch_id},
                                    {QStringLiteral("revision"), this->hash},
                                    {QStringLiteral("recordHash"), this->digest}}};
                                if (this->include_unborn_branch)
                                    heads.append(QJsonObject{
                                        {QStringLiteral("branch"), QStringLiteral("unborn")},
                                        {QStringLiteral("branchId"), this->unborn_branch_id},
                                        {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)},
                                        {QStringLiteral("recordHash"), QJsonValue(QJsonValue::Null)}});
                                const QJsonObject response{
                                    {QStringLiteral("revisions"), QJsonArray{revision}},
                                    {QStringLiteral("heads"), heads},
                                    {QStringLiteral("complete"), true},
                                    {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)}};
                                data = QJsonDocument(response).toJson();
                            }
                        }
                        else if (this->include_second_project
                            && path == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256")
                                     .arg(this->second_project_id).toUtf8())
                            data = QJsonDocument(QJsonObject{
                                {QStringLiteral("revisions"), QJsonArray{}},
                                {QStringLiteral("heads"), QJsonArray{QJsonObject{
                                    {QStringLiteral("branch"), QStringLiteral("main")},
                                    {QStringLiteral("branchId"), this->second_branch_id},
                                    {QStringLiteral("revision"), QJsonValue(QJsonValue::Null)},
                                    {QStringLiteral("recordHash"), QJsonValue(QJsonValue::Null)}}}},
                                {QStringLiteral("complete"), true},
                                {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)}}).toJson();
                        else if (!this->created_project.isEmpty() && path == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256")
                                     .arg(this->created_project.value(QStringLiteral("id")).toString()).toUtf8())
                            data = R"({"heads":[],"revisions":[],"complete":true,"nextCursor":null})";
                    }
                    socket->write("HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(data.size()) + "\r\n\r\n" + data);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
        return true;
    }

    QString endpoint() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(this->server.serverPort());
    }
};
}

int main(int argc, char **argv)
{
    QTemporaryDir config;
    if (!require(config.isValid(), "temporary profile store failed")) return 1;
    qputenv("XDG_DATA_HOME", config.path().toUtf8());
    // Configured connections live in the AOWIS-CONFIG store under the user
    // configuration location; never touch the developer's real one.
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
    FakeDvmdb first;
    FakeDvmdb second;
    second.repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d002");
    if (!require(first.start() && second.start(), "loopback servers failed")) return 1;

    ProjectServerProfile a;
    a.id = first.repository_id;
    a.name = QStringLiteral("Source A");
    a.connection_type = QStringLiteral("remote");
    a.auth_mode = QStringLiteral("None");
    a.endpoint = first.endpoint();
    ProjectServerProfile b = a;
    b.id = second.repository_id;
    b.name = QStringLiteral("Source B");
    b.endpoint = second.endpoint();
    QProcess peer;
    QString tunnel_profile_id;
    if (argc == 2)
    {
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        peer.start(QString::fromLocal8Bit(argv[1]), {QString::number(first.server.serverPort())});
        if (!require(peer.waitForStarted() && waitUntil(app, [&] { return peer.canReadLine(); }),
                     "WireGuard peer did not start")) return 1;
        const QJsonObject ready = QJsonDocument::fromJson(peer.readLine()).object();
        const QByteArray key = ready.value(QStringLiteral("client_private_key")).toString().toLatin1();
        const QByteArray public_key = ready.value(QStringLiteral("server_public_key")).toString().toLatin1();
        const QByteArray endpoint = ready.value(QStringLiteral("endpoint")).toString().toLatin1();
        const QByteArray config_text = "[Interface]\nAddress=10.173.91.2/32\nPrivateKey=" + key
            + "\n[Peer]\nPublicKey=" + public_key + "\nEndpoint=" + endpoint + "\nAllowedIPs=10.173.91.1/32\n";
        // Connections reference profiles stored in AOWIS-CONFIG by id.
        QString import_error;
        if (!require(importWireGuardProfile(QStringLiteral("Catalogue peer"), config_text, &tunnel_profile_id,
                                            &import_error), "could not store the WireGuard profile")) return 1;
        a.endpoint = QStringLiteral("http://10.173.91.1:18080");
        a.wireguard_profile_id = tunnel_profile_id;
#else
        if (!require(false, "the WireGuard catalogue test needs AOWIS_GUI_WIREGUARD_PROBE")) return 1;
#endif
    }
    if (!require(saveProjectServerProfiles({a, b}), "could not store multiple sources")) return 1;

    ProjectsWidget projects;
    projects.resize(1200, 740);
    projects.show();
    QTreeWidget *sources = projects.findChild<QTreeWidget *>(QStringLiteral("projectsTree"));
    QTreeWidget *tree = projects.findChild<QTreeWidget *>(QStringLiteral("projectsTree"));
    ProjectRevisionGraph *graph = projects.findChild<ProjectRevisionGraph *>(
        QStringLiteral("projectRevisionGraph"));
    if (!require(sources != nullptr && sources == tree && graph != nullptr,
                 "Explorer must use one tree for sources, projects and branches")) return 1;
    if (!require(waitUntil(app, [&]
        {
            return tree->topLevelItemCount() == 2
                && tree->topLevelItem(0)->childCount() == 1
                && tree->topLevelItem(1)->childCount() == 1;
        }, argc == 2 ? 15000 : 5000), "database catalogues did not load independently at startup")) return 1;
    if (!require(sources->topLevelItem(0)->checkState(0) == Qt::Checked
            && sources->topLevelItem(1)->checkState(0) == Qt::Checked,
            "both sources should be visible by default")) return 1;
    if (!require(tree->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toString()
                 != tree->topLevelItem(1)->child(0)->data(0, Qt::UserRole).toString(),
                 "project identities collide across different databases")) return 1;
    // Both fixtures serve the same project UUIDv7 from different
    // repositories: the graph shows them as clones with one dashed link.
    if (!require(waitUntil(app, [&] { return graph->cloneLinks().size() == 1; })
                 && graph->cloneLinks().first().first.startsWith(QStringLiteral("group:project:"))
                 && graph->cloneLinks().first().second.startsWith(QStringLiteral("group:project:"))
                 && graph->cloneLinks().first().first != graph->cloneLinks().first().second,
                 "the same project on two databases is not linked as a clone")) return 1;
    // Both fixtures hold the same revision: the dotted sync line joins them.
    if (!require(waitUntil(app, [&] { return graph->cloneSyncLinks().size() == 1; })
                 && graph->cloneSyncLinks().first().first.endsWith(QStringLiteral("|") + first.hash)
                 && graph->cloneSyncLinks().first().second.endsWith(QStringLiteral("|") + first.hash)
                 && graph->cloneSyncLinks().first().first != graph->cloneSyncLinks().first().second,
                 "the last common commit of the two clones is not linked")) return 1;
    // Clone project / Paste clone: the in-app clipboard drives Paste's state,
    // label and tooltip. Both fixtures hold the same project UUIDv7, so B
    // already has the clone and Paste becomes Update clone.
    {
        QPushButton *clone = projects.findChild<QPushButton *>(QStringLiteral("projectsInspectorCloneProject"));
        QPushButton *paste = projects.findChild<QPushButton *>(QStringLiteral("projectsInspectorPasteClone"));
        if (!require(clone != nullptr && paste != nullptr, "missing clone buttons")) return 1;
        sources->setCurrentItem(sources->topLevelItem(1));
        app.processEvents();
        if (!require(!paste->isHidden() && !paste->isEnabled()
                         && paste->text() == QStringLiteral("Paste clone")
                         && paste->toolTip().contains(QStringLiteral("Nothing to paste")),
                     "Paste clone is not greyed out with a reason while nothing is copied")) return 1;
        sources->setCurrentItem(sources->topLevelItem(0)->child(0));
        app.processEvents();
        if (!require(!clone->isHidden() && clone->isEnabled() && paste->isHidden(),
                     "a database project does not offer Clone project")) return 1;
        clone->click();
        app.processEvents();
        if (!require(clone->toolTip().startsWith(QStringLiteral("Copied")),
                     "Clone project does not confirm the copy")) return 1;
        sources->setCurrentItem(sources->topLevelItem(0));
        app.processEvents();
        if (!require(!paste->isEnabled() && paste->toolTip().contains(QStringLiteral("comes from this database")),
                     "Paste clone is offered on the copied project's own database")) return 1;
        sources->setCurrentItem(sources->topLevelItem(1));
        app.processEvents();
        if (!require(paste->isEnabled() && paste->text() == QStringLiteral("Update clone")
                         && paste->toolTip().contains(QStringLiteral("already has")),
                     "a database that already holds the clone does not offer Update clone")) return 1;
    }
    // The source profile name is not a globally unique repository identity.
    sources->setCurrentItem(sources->topLevelItem(0));
    app.processEvents();
    const auto inspectorHasIdentity = [&projects](const QString &identity)
    {
        for (QLabel *label : projects.findChildren<QLabel *>())
            if (label->text().contains(QStringLiteral("DVMDB repository UUIDv7: ") + identity))
                return true;
        return false;
    };
    if (!require(inspectorHasIdentity(first.repository_id),
                 "first database UUID was not shown in inspector")) return 1;
    // An invalid revision graph is a catalogue failure, not an offline server.
    // /projects remains valid, so the independent health monitor must keep the
    // connection available while the UI reports the precise catalogue error.
    QLabel *health_label = projects.findChild<QLabel *>(
        QStringLiteral("projectsDatabaseHealthStatus"));
    QLabel *source_status = projects.findChild<QLabel *>(
        QStringLiteral("serversPreviewStatus"));
    QPushButton *refresh = projects.findChild<QPushButton *>(
        QStringLiteral("projectsSourceRefreshButton"));
    if (!require(health_label != nullptr && source_status != nullptr && refresh != nullptr,
                 "missing connection-health or catalogue-status controls")) return 1;
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available");
        }), "healthy service never reached Available state")) return 1;
    QLabel *health_icon = projects.findChild<QLabel *>(
        QStringLiteral("projectsDatabaseHealthIndicator"));
    if (!require(health_icon != nullptr,
                 "server inspector health indicator is missing")) return 1;
    int unexpected_tree_edits = 0;
    QObject::connect(sources, &QTreeWidget::itemChanged, &projects,
        [&unexpected_tree_edits](QTreeWidgetItem *, int) { ++unexpected_tree_edits; });
    // Sidebar and inspector must consume the same availability snapshot.
    // Neither choosing a card nor drawing the graph should trigger another
    // service probe; the sole remote probe belongs to HttpHealthMonitor.
    const QColor sidebar_green = sources->topLevelItem(0)->icon(1)
        .pixmap(16, 16).toImage().pixelColor(8, 8);
    const QColor inspector_green = health_icon->grab().toImage().pixelColor(8, 8);
    if (!require(sidebar_green.green() > sidebar_green.red()
                     && inspector_green.green() > inspector_green.red()
                     && sidebar_green == inspector_green,
                 "sidebar and inspector do not share the green server status")) return 1;
    if (argc == 2)
    {
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        // A second tunnel would have to reread the profile. Deleting the
        // stored profile proves that Verify and Refresh reuse the active one.
        bool removed = false;
        if (const aowis::config::WireGuardProfiles *stored = guiWireGuardProfiles())
        {
            const aowis::config::Result<aowis::config::ObjectId> stored_id =
                aowis::config::ObjectId::parse(tunnel_profile_id.toStdString());
            if (stored_id)
            {
                const aowis::config::Result<aowis::config::WireGuardProfile> current = stored->read(stored_id.value());
                removed = current.hasValue()
                    && stored->erase(stored_id.value(), current.value().summary.info.revision,
                           [](const aowis::config::OwnerSession &, const aowis::config::ObjectId &) {
                               return aowis::config::Result<std::size_t>::success(0);
                           }).hasValue();
            }
        }
        if (!require(removed, "could not remove the stored WireGuard profile")) return 1;
#endif
        GroupBoxCollapsible *group = projects.findChild<GroupBoxCollapsible *>(QStringLiteral("projectWireGuardGroup"));
        if (!require(group != nullptr, "missing WireGuard inspector group")) return 1;
        group->setCollapsed(false);
        QPushButton *verify = projects.findChild<QPushButton *>(QStringLiteral("projectsVerifyRepository"));
        if (!require(verify != nullptr && verify->isEnabled(), "missing saved-server verification")) return 1;
        verify->click();
        if (!require(waitUntil(app, [&] {
            QMessageBox *message = projects.findChild<QMessageBox *>();
            return message != nullptr && message->text().contains(first.repository_id);
        }), "saved verification did not reuse the connected tunnel")) return 1;
        projects.findChild<QMessageBox *>()->accept();
    }
    sources->topLevelItem(0)->setExpanded(false);
    first.graph_valid = false;
    refresh->click();
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available · catalogue error")
                && source_status->text().contains(QStringLiteral("Invalid DVMDB revision graph page"));
        }), "catalogue failure incorrectly marked the responding server unavailable")) return 1;
    if (!require(!sources->topLevelItem(0)->isExpanded(),
                 "a catalogue refresh reopened a manually collapsed database")) return 1;
    sources->topLevelItem(0)->setExpanded(true);
    const QColor sidebar_amber = sources->topLevelItem(0)->icon(1)
        .pixmap(16, 16).toImage().pixelColor(8, 8);
    const QColor inspector_amber = health_icon->grab().toImage().pixelColor(8, 8);
    if (!require(sidebar_amber == inspector_amber
                     && sidebar_amber != sidebar_green
                     && sidebar_amber.red() > sidebar_amber.green()
                     && unexpected_tree_edits == 0,
                 "catalogue health changes must share amber status without editing server profiles")) return 1;
    if (!require(tree->topLevelItemCount() == 2,
                 "catalogue failure discarded a previously loaded project snapshot")) return 1;
    first.graph_valid = true;
    refresh->click();
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available")
                && source_status->text().contains(QStringLiteral("catalogue loaded"));
        }), "healthy catalogue did not recover after a refresh")) return 1;
    // A broken /projects response must not fail the independent identity
    // probe. The source stays reachable and recovery requires no reconnect.
    first.catalogue_valid = false;
    refresh->click();
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available · catalogue error")
                && source_status->text().contains(QStringLiteral("Invalid or oversized DVMDB project list"));
        }), "project-list failure incorrectly marked the server offline")) return 1;
    first.catalogue_valid = true;
    refresh->click();
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available")
                && source_status->text().contains(QStringLiteral("catalogue loaded"));
        }), "project-list catalogue did not recover")) return 1;
    sources->setCurrentItem(sources->topLevelItem(1));
    app.processEvents();
    if (!require(inspectorHasIdentity(second.repository_id),
                 "second database UUID was not shown separately")) return 1;
    tree->setCurrentItem(tree->topLevelItem(1)->child(0));
    app.processEvents();
    // Network editing belongs to a selected HEAD/Revision Overview; a project
    // must not expose the old ambiguous Edit network action.
    QStackedWidget *action_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorActionsStack"));
    QWidget *active_actions = action_stack != nullptr ? action_stack->currentWidget() : nullptr;
    QStackedWidget *overview_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorOverviewStack"));
    QWidget *active_overview = overview_stack != nullptr ? overview_stack->currentWidget() : nullptr;
    QPushButton *import_network = active_overview != nullptr
        ? active_overview->findChild<QPushButton *>(QStringLiteral("projectsInspectorImport"))
        : nullptr;
    if (!require(active_actions != nullptr
                     && active_actions->objectName() == QStringLiteral("projectsProjectInspector")
                     && active_overview != nullptr
                     && active_overview->objectName() == QStringLiteral("projectsProjectOverviewForm")
                     && import_network != nullptr && import_network->isVisible()
                     && active_overview->findChild<QPushButton *>(
                         QStringLiteral("projectsInspectorEditNetwork")) == nullptr,
                 "database Project Overview still exposes ambiguous network editing")) return 1;
    QLabel *metadata = overview_stack != nullptr && overview_stack->currentWidget() != nullptr
        ? overview_stack->currentWidget()->findChild<QLabel *>(
            QStringLiteral("projectInspectorMetadata")) : nullptr;
    if (!require(metadata != nullptr
                 && metadata->text().contains(QStringLiteral("Repository UUIDv7: ") + second.repository_id),
                 "project inspector lost the repository-qualified project identity")) return 1;
    const QString first_graph_id = first.repository_id + QStringLiteral("|") + first.project_id + QStringLiteral("|") + first.hash;
    const QString second_graph_id = second.repository_id + QStringLiteral("|") + second.project_id + QStringLiteral("|") + second.hash;
    graph->fitGraph();
    // Immutable revisions have no exclusive owner in DVMDB. A single
    // reachable branch must still be identified, never "Branch: Unknown".
    if (!require(graph->nodeBranchCaption(first_graph_id) == QStringLiteral("Branch: main")
                 && graph->nodeBranchCaption(second_graph_id) == QStringLiteral("Branch: main"),
                 "database revisions lost their reachable branch labels")) return 1;
    if (!require(!graph->revisionViewportPosition(first_graph_id).isNull()
                 && !graph->revisionViewportPosition(second_graph_id).isNull(),
                 "aggregate graph omitted a source or merged same-hash revisions")) return 1;
    // Neither database is personal, so neither has a HEAD card: each source's
    // branch reference resolves to its own tip revision card, even when two
    // databases have identical revision content.
    const QString first_branch = QStringLiteral("group:branch:") + first.repository_id + QStringLiteral("|") + first.project_id
        + QStringLiteral(":") + first.branch_id;
    const QString second_branch = QStringLiteral("group:branch:") + second.repository_id + QStringLiteral("|") + second.project_id
        + QStringLiteral(":") + second.branch_id;
    if (!require(graph->nodeTitle(first_branch).isEmpty() && graph->nodeTitle(second_branch).isEmpty()
                 && graph->nodeViewportPosition(first_branch) == graph->revisionViewportPosition(first_graph_id)
                 && graph->nodeViewportPosition(second_branch) == graph->revisionViewportPosition(second_graph_id),
                 "non-personal branches must resolve to their own tip revision cards")) return 1;
    const QPointF first_server = graph->nodeViewportPosition(QStringLiteral("group:server:") + first.repository_id);
    const QPointF second_server = graph->nodeViewportPosition(QStringLiteral("group:server:") + second.repository_id);
    const QPointF first_project = graph->nodeViewportPosition(
        QStringLiteral("group:project:") + first.repository_id + QStringLiteral("|") + first.project_id);
    if (!require(!first_server.isNull() && !second_server.isNull() && !first_project.isNull(),
                 "graph must expose a separate node for each database and project")) return 1;
    if (!require(first_server.y() < first_project.y()
                 && first_project.y() < graph->revisionViewportPosition(first_graph_id).y(),
                 "database/project hierarchy is not above its revisions")) return 1;
    const double separation = std::abs(first_server.x() - second_server.x()) / graph->zoomFactor();
    if (!require(separation >= 280.0 && separation <= 400.0,
                 "single-project databases are spaced too far apart")) return 1;
    if (!require(projects.findChild<QPushButton *>(QStringLiteral("projectsSourceConnectButton")) == nullptr,
                 "legacy manual connect/disconnect control still exists")) return 1;

    // The toolbar chooses a real project/branch, not a revision preview.
    // With a single branch its context is unambiguous; without a personal
    // database it must fail safely rather than opening anything implicitly.
    QString toolbar_project;
    QString toolbar_branch;
    QString open_error;
    QObject::connect(&projects, &ProjectsWidget::projectContextChanged, &projects,
        [&](const QStringList &, const QString &project_name,
            const QStringList &, const QString &branch_name)
    {
        toolbar_project = project_name;
        toolbar_branch = branch_name;
    });
    QObject::connect(&projects, &ProjectsWidget::networkOpenProgress, &projects,
        [&](const QString &message, bool failed)
    {
        if (failed) open_error = message;
    });
    projects.selectToolbarProject(QStringLiteral("Source A / Shared name"));
    if (!require(toolbar_project == QStringLiteral("Source A / Shared name")
            && toolbar_branch == QStringLiteral("main")
            && !open_error.isEmpty(),
            "single-branch project did not select the branch or fail safely without personal DVMDB"))
        return 1;
    open_error.clear();
    projects.selectToolbarBranch(QStringLiteral("[none]"));
    if (!require(open_error.isEmpty(), "[none] unexpectedly requested a network open")) return 1;

    sources->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
    app.processEvents();
    if (!require(tree->topLevelItemCount() == 2
                 && tree->topLevelItem(1)->childCount() == 0
                 && tree->topLevelItem(1)->checkState(0) == Qt::Unchecked
                 && loadProjectServerProfiles().at(1).visible == false
                 && graph->revisionViewportPosition(second_graph_id).isNull(),
                 "unchecking a source must hide its projects but retain the checkbox")) return 1;
    if (!require(graph->nodeViewportPosition(QStringLiteral("group:server:") + second.repository_id).isNull(),
                 "hidden database still has a graph header")) return 1;
    sources->topLevelItem(1)->setCheckState(0, Qt::Checked);
    app.processEvents();
    if (!require(tree->topLevelItemCount() == 2
                 && tree->topLevelItem(1)->childCount() == 1
                 && loadProjectServerProfiles().at(1).visible,
                 "rechecking a source did not restore catalogue visibility")) return 1;

    // A successful HTTP status alone is insufficient: validate service data,
    // degrade after the first bad response and turn offline after the second.
    FakeDvmdb monitor_fixture;
    if (!require(monitor_fixture.start(), "health monitor fixture failed to start")) return 1;
    HttpHealthMonitor monitor;
    HttpHealthMonitor::State status = HttpHealthMonitor::State::Checking;
    monitor.start(QUrl(monitor_fixture.endpoint() + QStringLiteral("/dvmdb/v1/projects")),
        [](const QByteArray &body)
        {
            const QJsonDocument json = QJsonDocument::fromJson(body);
            return json.isObject() && json.object().value(QStringLiteral("projects")).isArray();
        }, [&](HttpHealthMonitor::State value, const QString &) { status = value; }, {}, 30000);
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Healthy; }),
                 "health probe did not validate a good service response")) return 1;
    monitor_fixture.valid = false;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Degraded; }),
                 "health probe did not degrade after invalid response")) return 1;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Offline; }),
                 "health probe did not declare offline after repeated failures")) return 1;
    monitor_fixture.valid = true;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Healthy; }),
                 "health probe did not recover after service restoration")) return 1;
    monitor.stop();

    if (argc == 2)
    {
        sources->setCurrentItem(sources->topLevelItem(0));
        sources->itemClicked(sources->topLevelItem(0), 0);
        if (!require(!health_label->parentWidget()->isHidden() && inspectorHasIdentity(first.repository_id)
                     && health_label->text() == QStringLiteral("Available"),
                     "clicking Source A must open its live inspector before stopping the peer")) return 1;
        QPushButton *create = projects.findChild<QPushButton *>(QStringLiteral("projectsInspectorCreateProject"));
        QLineEdit *name = projects.findChild<QLineEdit *>(QStringLiteral("projectsProjectCreationName"));
        if (!require(create != nullptr && create->isEnabled() && name != nullptr, "missing create project controls")) return 1;
        create->click();
        name->setText(QStringLiteral("Tunnel project"));
        QPushButton *confirm = action_stack->currentWidget()->findChild<QPushButton *>(QStringLiteral("projectsCreationConfirm"));
        if (!require(confirm != nullptr && confirm->isEnabled(), "project creation form was not ready")) return 1;
        confirm->click();
        if (!require(waitUntil(app, [&] {
            return first.tracking_requests > 0 && first.created_project.value(QStringLiteral("name")).toString()
                == QStringLiteral("Tunnel project") && tree->topLevelItemCount() > 0 && tree->topLevelItem(0)->childCount() == 2;
        }), "tunneled Create project did not verify, write and refresh")) return 1;
        if (!require(second.created_project.isEmpty(), "write reached the other repository")) return 1;
        sources->setCurrentItem(sources->topLevelItem(0));
        sources->itemClicked(sources->topLevelItem(0), 0);
        QCheckBox *personal = projects.findChild<QCheckBox *>(QStringLiteral("projectsPersonalServerCheckbox"));
        if (!require(personal != nullptr && personal->isEnabled(), "personal designation unavailable")) return 1;
        const std::function<bool()> saved_personal = [&first] {
            for (const ProjectServerProfile &profile : loadProjectServerProfiles())
                if (profile.id == first.repository_id) return profile.personal;
            return false;
        };
        const int previous_tracking = first.tracking_requests;
        personal->click();
        if (!require(waitUntil(app, saved_personal) && first.role_posts == 1
                     && first.tracking_requests > previous_tracking && second.role_posts == 0,
                     "personal designation must verify and configure only the tunneled repository")) return 1;
        personal->click();
        if (!require(!saved_personal(), "personal designation was not cleared locally")) return 1;
        personal->click();
        if (!require(waitUntil(app, saved_personal) && first.role_posts == 1,
                     "already-private tunneled repository was reconfigured")) return 1;
        peer.closeWriteChannel();
        if (!require(peer.waitForFinished(5000) && peer.exitCode() == 0, "test peer did not stop")) return 1;
        if (!waitUntil(app, [&] { return health_label->text() == QStringLiteral("Unavailable"); }, 25000))
        {
            std::fprintf(stderr, "Source A after peer shutdown: inspector=%s, sidebar=%s\n",
                         qPrintable(health_label->text()), qPrintable(sources->topLevelItem(0)->toolTip(0)));
            return 1;
        }
        sources->setCurrentItem(sources->topLevelItem(1));
        const int previous_requests = second.catalogue_requests;
        refresh->click();
        if (!require(waitUntil(app, [&] {
            return second.catalogue_requests > previous_requests && health_label->text() == QStringLiteral("Available")
                && source_status->text().contains(QStringLiteral("catalogue loaded"));
        }), "direct catalogue must still refresh after WireGuard peer loss")) return 1;
    }

    // A saved offline endpoint begins with a temporary UUIDv7 and becomes the
    // authoritative repository UUID after its first successful catalogue read.
    first.created_project = {};
    ProjectServerProfile pending;
    pending.id = DvmdbIdentity::newUuidV7();
    pending.pending = true;
    pending.name = QStringLiteral("Pending verification");
    pending.endpoint = first.endpoint();
    if (!require(saveProjectServerProfiles({pending}),
                 "failed to save an unverified pending database")) return 1;
    ProjectsWidget pendingProjects;
    pendingProjects.show();
    if (!require(waitUntil(app, [&]
        {
            const QList<ProjectServerProfile> profiles = loadProjectServerProfiles();
            return profiles.size() == 1 && !profiles.first().pending
                && profiles.first().id == first.repository_id;
        }), "pending connection did not promote to the real repository UUID")) return 1;
    QTreeWidget *promotedTree = pendingProjects.findChild<QTreeWidget *>(
        QStringLiteral("projectsTree"));
    if (!require(waitUntil(app, [&]
        {
            return promotedTree != nullptr && promotedTree->topLevelItemCount() == 1
                && promotedTree->topLevelItem(0)->childCount() == 1;
        }), "promoted repository did not recover its project catalogue")) return 1;

    // An independent HEAD shortcut catalogue: no selected-project filter,
    // no visibility dependency, no immutable revision entries or extra probes.
    // The same reader/selection/open path drives both the graph and shortcuts.
    {
        FakeDvmdb personal_backend;
        personal_backend.repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d099");
        personal_backend.include_unborn_branch = true;
        personal_backend.include_second_project = true;
        personal_backend.role = QStringLiteral("private");
        if (!require(personal_backend.start(), "personal shortcut fixture failed")) return 1;
        ProjectServerProfile personal_profile = a;
        personal_profile.id = personal_backend.repository_id;
        personal_profile.name = QStringLiteral("Personal Fixture");
        personal_profile.endpoint = personal_backend.endpoint();
        personal_profile.wireguard_profile_id.clear();
        personal_profile.personal = true;
        ProjectServerProfile read_only_profile = b;
        read_only_profile.personal = false;
        if (!require(saveProjectServerProfiles({personal_profile, read_only_profile}),
                     "HEAD shortcut profile setup failed")) return 1;
        ProjectsWidget shortcuts;
        shortcuts.resize(1200, 740);
        shortcuts.show();
        QTreeWidget *heads = shortcuts.findChild<QTreeWidget *>(
            QStringLiteral("editableHeadsTree"));
        QTreeWidget *explorer = shortcuts.findChild<QTreeWidget *>(
            QStringLiteral("projectsTree"));
        ProjectRevisionGraph *shortcut_graph = shortcuts.findChild<ProjectRevisionGraph *>(
            QStringLiteral("projectRevisionGraph"));
        if (!require(waitUntil(app, [&] {
            return heads != nullptr && explorer != nullptr && shortcut_graph != nullptr
                && heads->topLevelItemCount() == 3
                && explorer->topLevelItemCount() == 2
                && explorer->topLevelItem(0)->childCount() == 2;
        }), "personal HEAD list did not include all branches and unborn HEADs")) return 1;
        GroupBoxCollapsible *explorer_section = shortcuts.findChild<GroupBoxCollapsible *>(
            QStringLiteral("explorerCollapsibleGroup"));
        GroupBoxCollapsible *heads_section = shortcuts.findChild<GroupBoxCollapsible *>(
            QStringLiteral("editableHeadsCollapsibleGroup"));
        QVBoxLayout *sidebar_layout = explorer_section != nullptr
            ? qobject_cast<QVBoxLayout *>(explorer_section->parentWidget()->layout()) : nullptr;
        if (!require(explorer_section != nullptr && heads_section != nullptr
                && sidebar_layout != nullptr
                && explorer_section->geometry().top() < heads_section->geometry().top()
                && sidebar_layout->stretch(sidebar_layout->indexOf(explorer_section)) == 2
                && sidebar_layout->stretch(sidebar_layout->indexOf(heads_section)) == 1
                && heads->minimumHeight() <= 110
                && explorer_section->height() > heads_section->height(),
                "populated HEAD shortcuts must sit below Explorer at a one-third height share"))
            return 1;
        heads_section->setCollapsed(true);
        app.processEvents();
        if (!require(sidebar_layout->stretch(sidebar_layout->indexOf(explorer_section)) == 2
                && sidebar_layout->stretch(sidebar_layout->indexOf(heads_section)) == 0,
                "collapsed HEAD shortcuts still claim sidebar height")) return 1;
        heads_section->setCollapsed(false);
        app.processEvents();
        if (!require(sidebar_layout->stretch(sidebar_layout->indexOf(heads_section)) == 1,
                "expanding HEAD shortcuts did not restore the one-third share")) return 1;
        QTreeWidgetItem *committed = nullptr;
        QTreeWidgetItem *unborn = nullptr;
        QTreeWidgetItem *other_project = nullptr;
        for (int i = 0; i < heads->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem *item = heads->topLevelItem(i);
            const QString key = item->data(0, Qt::UserRole).toString();
            if (key.endsWith(personal_backend.branch_id)) committed = item;
            if (key.endsWith(personal_backend.unborn_branch_id)) unborn = item;
            if (key.endsWith(personal_backend.second_branch_id)) other_project = item;
        }
        if (!require(committed != nullptr && unborn != nullptr && other_project != nullptr
                && committed->text(0).contains(QStringLiteral("Shared name / main"))
                && unborn->text(0).contains(QStringLiteral("No commits yet"))
                && other_project->text(0).contains(QStringLiteral("Other personal project"))
                && heads->topLevelItemCount() == 3,
                "shortcuts lost per-branch identity, unborn HEADs or project context")) return 1;
        const int catalogue_requests = personal_backend.catalogue_requests;
        explorer->topLevelItem(0)->setExpanded(false);
        explorer->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        app.processEvents();
        if (!require(heads->topLevelItemCount() == 3,
                     "hiding/collapsing a personal source removed its HEAD shortcuts")) return 1;
        // Re-enable the graph before checking focus, without expanding Explorer.
        explorer->topLevelItem(0)->setCheckState(0, Qt::Checked);
        app.processEvents();
        other_project = nullptr;
        for (int index = 0; index < heads->topLevelItemCount(); ++index)
            if (heads->topLevelItem(index)->data(0, Qt::UserRole).toString()
                .endsWith(personal_backend.second_branch_id))
                other_project = heads->topLevelItem(index);
        if (!require(other_project != nullptr, "HEAD shortcut vanished after re-enabling source")) return 1;
        heads->setCurrentItem(other_project);
        app.processEvents();
        if (!require(shortcut_graph->selectionId()
                         == QStringLiteral("group:branch:") + other_project->data(0, Qt::UserRole).toString().mid(2)
                         && shortcut_graph->nodeViewportPosition(shortcut_graph->selectionId()) != QPointF()
                         && shortcuts.findChild<QStackedWidget *>(
                             QStringLiteral("projectsInspectorActionsStack")) != nullptr
                         && shortcuts.findChild<QStackedWidget *>(
                             QStringLiteral("projectsInspectorActionsStack"))->currentWidget()->objectName()
                                == QStringLiteral("projectsRevisionInspector"),
                     "cross-project HEAD selection did not reach the graph and inspector")) return 1;
        QString opened_branch;
        bool checkout_requested = true;
        QObject::connect(&shortcuts, &ProjectsWidget::networkOpenRequested, &shortcuts,
            [&opened_branch, &checkout_requested](const QString &, const QString &branch_id,
                const QString &, const QString &, const QString &, bool checkout)
            { opened_branch = branch_id; checkout_requested = checkout; });
        heads->itemDoubleClicked(other_project, 0);
        app.processEvents();
        if (!require(opened_branch == other_project->data(0, Qt::UserRole).toString().mid(2)
                         && !checkout_requested,
                     "double-click on a personal HEAD did not use the standard network open path")) return 1;
        if (!require(personal_backend.catalogue_requests == catalogue_requests,
                     "shortcut navigation triggered an unnecessary catalogue refresh")) return 1;
    }

    // A HEAD is the writable working state of the personal database only.
    // Elsewhere a branch with commits is its tip revision (labelled
    // "Branch: <name>"), a branch without commits is a plain branch card, and
    // selecting either inspects the Branch: no Commit, no Discard. Editing the
    // tip still opens the branch, checked out into the personal database.
    {
        FakeDvmdb own;
        own.repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d0a1");
        own.role = QStringLiteral("private");
        FakeDvmdb shared;
        shared.repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d0a2");
        shared.role = QStringLiteral("shared");
        shared.include_unborn_branch = true;
        // The shared clone's main is at another revision than the personal
        // one, so the personal revision is not there yet.
        shared.hash = QStringLiteral("018f84c0-0000-7000-8000-00000000b0a2");
        own.include_second_project = true;
        // A second upstream of the same project, also behind the personal one.
        FakeDvmdb shared_two;
        shared_two.repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d0a3");
        shared_two.role = QStringLiteral("shared");
        shared_two.hash = QStringLiteral("018f84c0-0000-7000-8000-00000000b0a3");
        if (!require(own.start() && shared.start() && shared_two.start(), "HEAD visibility fixtures failed")) return 1;
        ProjectServerProfile own_profile = a;
        own_profile.id = own.repository_id;
        own_profile.name = QStringLiteral("Own");
        own_profile.endpoint = own.endpoint();
        own_profile.wireguard_profile_id.clear();
        own_profile.personal = true;
        ProjectServerProfile shared_profile = a;
        shared_profile.id = shared.repository_id;
        shared_profile.name = QStringLiteral("Team");
        shared_profile.endpoint = shared.endpoint();
        shared_profile.wireguard_profile_id.clear();
        shared_profile.personal = false;
        ProjectServerProfile shared_two_profile = shared_profile;
        shared_two_profile.id = shared_two.repository_id;
        shared_two_profile.name = QStringLiteral("Team 2");
        shared_two_profile.endpoint = shared_two.endpoint();
        if (!require(saveProjectServerProfiles({own_profile, shared_profile, shared_two_profile}),
                     "HEAD visibility profile setup failed")) return 1;
        ProjectsWidget heads_widget;
        heads_widget.resize(1200, 740);
        heads_widget.show();
        ProjectRevisionGraph *heads_graph = heads_widget.findChild<ProjectRevisionGraph *>(
            QStringLiteral("projectRevisionGraph"));
        const QString own_project = own.repository_id + QStringLiteral("|") + own.project_id;
        const QString shared_project = shared.repository_id + QStringLiteral("|") + shared.project_id;
        const QString own_tip = own_project + QStringLiteral("|") + own.hash;
        const QString shared_tip = shared_project + QStringLiteral("|") + shared.hash;
        const QString own_main = own_project + QStringLiteral(":") + own.branch_id;
        const QString shared_main = shared_project + QStringLiteral(":") + shared.branch_id;
        const QString shared_unborn = shared_project + QStringLiteral(":") + shared.unborn_branch_id;
        if (!require(waitUntil(app, [&] {
                return heads_graph != nullptr
                    && !heads_graph->nodeTitle(own_tip).isEmpty()
                    && !heads_graph->nodeTitle(shared_tip).isEmpty()
                    && !heads_graph->nodeTitle(shared_two.repository_id + QStringLiteral("|") + shared_two.project_id
                        + QStringLiteral("|") + shared_two.hash).isEmpty();
            }), "HEAD visibility graph did not load both databases")) return 1;
        if (!require(heads_graph->nodeTitle(QStringLiteral("group:branch:") + own_main) == QStringLiteral("HEAD"),
                     "the personal database lost its HEAD card")) return 1;
        if (!require(heads_graph->nodeTitle(QStringLiteral("group:branch:") + shared_main).isEmpty()
                         && heads_graph->nodeBranchCaption(shared_tip) == QStringLiteral("Branch: main"),
                     "a shared database still shows a HEAD card instead of labelling the tip")) return 1;
        if (!require(heads_graph->nodeTitle(QStringLiteral("group:branch:") + shared_unborn) == QStringLiteral("unborn")
                         && heads_graph->nodeBranchCaption(QStringLiteral("group:branch:") + shared_unborn)
                             == QStringLiteral("Branch · no commits yet"),
                     "a shared branch without commits is not a plain branch card")) return 1;
        // The branch reference resolves to the tip card, so navigation and
        // a later Push can address the branch without a HEAD.
        if (!require(heads_graph->nodeViewportPosition(QStringLiteral("group:branch:") + shared_main)
                         == heads_graph->revisionViewportPosition(shared_tip),
                     "a shared branch reference does not resolve to its tip revision")) return 1;
        QPushButton *commit = heads_widget.findChild<QPushButton *>(QStringLiteral("projectsHeadInspectorCommit"));
        emit heads_graph->headActivated(shared_unborn);
        app.processEvents();
        if (!require(heads_graph->selectionId() == QStringLiteral("group:branch:") + shared_unborn
                         && commit != nullptr && !commit->isVisible(),
                     "a shared branch card opened a HEAD inspector with Commit")) return 1;
        emit heads_graph->headActivated(shared_main);
        app.processEvents();
        if (!require(heads_graph->selectionId() == shared_tip && !commit->isVisible(),
                     "selecting a shared branch did not select its tip revision")) return 1;
        emit heads_graph->headActivated(own_main);
        app.processEvents();
        if (!require(heads_graph->selectionId() == QStringLiteral("group:branch:") + own_main,
                     "the personal HEAD is no longer selectable")) return 1;

        // Push lives in the personal database: HEAD pushes its branch to the
        // database with the same project, and says so before sending.
        QPushButton *push = heads_widget.findChild<QPushButton *>(QStringLiteral("projectsRevisionInspectorPush"));
        if (!require(push != nullptr && !push->isHidden() && push->isEnabled()
                         && push->toolTip().contains(QStringLiteral("every commit")),
                     "a personal HEAD with a shared clone cannot be pushed")) return 1;
        push->click();
        app.processEvents();
        QDialog *push_dialog = heads_widget.findChild<QDialog *>(QStringLiteral("projectsPushDialog"));
        QLabel *push_text = push_dialog == nullptr ? nullptr
            : push_dialog->findChild<QLabel *>(QStringLiteral("projectsPushExplanation"));
        QComboBox *push_branch = push_dialog == nullptr ? nullptr
            : push_dialog->findChild<QComboBox *>(QStringLiteral("projectsPushBranch"));
        QList<QCheckBox *> push_targets = push_dialog == nullptr ? QList<QCheckBox *>()
            : push_dialog->findChildren<QCheckBox *>(QStringLiteral("projectsPushTarget"));
        QDialogButtonBox *push_buttons = push_dialog == nullptr ? nullptr
            : push_dialog->findChild<QDialogButtonBox *>(QStringLiteral("projectsPushButtons"));
        // Every upstream of the project is offered and ticked: push to all.
        if (!require(push_dialog != nullptr && push_dialog->isVisible() && push_text != nullptr
                         && push_text->text().contains(QStringLiteral("'main' to 'Team', 'Team 2'"))
                         && push_text->text().contains(QStringLiteral("Uncommitted working changes"))
                         && push_branch != nullptr && push_branch->count() == 1 && !push_branch->isVisibleTo(push_dialog)
                         && push_targets.size() == 2 && push_targets.at(0)->isChecked()
                         && push_targets.at(1)->isChecked() && push_targets.at(0)->isVisibleTo(push_dialog)
                         && push_buttons != nullptr && push_buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                     "the HEAD push dialog does not offer all upstreams of the project")) return 1;
        // Or only to the ticked ones; none ticked cannot be pushed.
        push_targets.at(0)->setChecked(false);
        if (!require(push_text->text().contains(QStringLiteral("'main' to 'Team 2'?"))
                         && push_buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                     "unticking an upstream did not leave only the other")) return 1;
        push_targets.at(1)->setChecked(false);
        if (!require(!push_buttons->button(QDialogButtonBox::Ok)->isEnabled()
                         && push_text->text().contains(QStringLiteral("Tick at least one")),
                     "a push without any upstream could be confirmed")) return 1;
        push_dialog->reject();
        app.processEvents();
        // A committed HEAD without uncommitted changes has nothing to discard.
        QPushButton *discard = heads_widget.findChild<QPushButton *>(QStringLiteral("projectsRevisionOverviewDelete"));
        if (!require(discard != nullptr && discard->text() == QStringLiteral("Discard uncommitted changes…")
                         && !discard->isEnabled()
                         && discard->toolTip().contains(QStringLiteral("Nothing to discard")),
                     "a clean HEAD offers to discard changes")) return 1;
        // A shared database has no Push; a personal branch without commits
        // or a project no other database has cannot be pushed.
        emit heads_graph->headActivated(shared_unborn);
        app.processEvents();
        if (!require(push->isHidden(), "a shared database offers Push")) return 1;
        const QString own_other = own.repository_id + QStringLiteral("|") + own.second_project_id
            + QStringLiteral(":") + own.second_branch_id;
        emit heads_graph->headActivated(own_other);
        app.processEvents();
        if (!require(!push->isHidden() && !push->isEnabled()
                         && push->toolTip().contains(QStringLiteral("no commits")),
                     "an unborn personal HEAD did not explain why it cannot be pushed")) return 1;

        // Opening the shared tip offers editing the branch itself.
        heads_graph->revisionActivated(shared_tip);
        app.processEvents();
        if (!require(heads_graph->selectionId() == shared_tip, "shared tip revision was not selectable")) return 1;
        QPushButton *open_network = heads_widget.findChild<QPushButton *>(
            QStringLiteral("projectsNetworkOverviewOpen"));
        if (!require(open_network != nullptr, "shared tip lacks Open network")) return 1;
        open_network->setEnabled(true); // The fixture has no hydraulic summary.
        open_network->click();
        app.processEvents();
        QDialog *open_dialog = heads_widget.findChild<QDialog *>(QStringLiteral("projectsRevisionOpenDialog"));
        QRadioButton *edit_branch = open_dialog == nullptr ? nullptr
            : open_dialog->findChild<QRadioButton *>(QStringLiteral("projectsRevisionOpenEditBranch"));
        QDialogButtonBox *open_buttons = open_dialog == nullptr ? nullptr
            : open_dialog->findChild<QDialogButtonBox *>(QStringLiteral("projectsRevisionOpenButtons"));
        if (!require(edit_branch != nullptr && edit_branch->isChecked()
                         && edit_branch->text().contains(QStringLiteral("main"))
                         && open_buttons != nullptr,
                     "a shared tip does not offer editing its branch")) return 1;
        QString opened_branch;
        bool checkout = false;
        QObject::connect(&heads_widget, &ProjectsWidget::networkOpenRequested, &heads_widget,
            [&opened_branch, &checkout](const QString &, const QString &branch_id,
                const QString &, const QString &, const QString &, bool remote)
            { opened_branch = branch_id; checkout = remote; });
        open_buttons->accepted();
        app.processEvents();
        if (!require(opened_branch == shared_main && checkout,
                     "editing a shared branch did not check it out into the personal database")) return 1;

        // An older tip, or the personal database, keeps the original choices.
        heads_graph->revisionActivated(own_tip);
        app.processEvents();
        if (!require(heads_graph->selectionId() == own_tip, "personal tip revision was not selectable")) return 1;
        if (!require(!push->isHidden() && push->isEnabled()
                         && push->toolTip().contains(QStringLiteral("every earlier one")),
                     "a personal revision cannot be pushed")) return 1;
        push->click();
        app.processEvents();
        push_dialog = nullptr;
        for (QDialog *dialog : heads_widget.findChildren<QDialog *>(QStringLiteral("projectsPushDialog")))
            if (dialog->isVisible()) push_dialog = dialog;
        push_text = push_dialog == nullptr ? nullptr
            : push_dialog->findChild<QLabel *>(QStringLiteral("projectsPushExplanation"));
        if (!require(push_text != nullptr && push_text->text().contains(own.hash)
                         && push_text->text().contains(QStringLiteral("Later revisions stay only here")),
                     "the revision push dialog does not name the revision and what stays")) return 1;
        push_dialog->reject();
        app.processEvents();
        open_network->setEnabled(true);
        open_network->click();
        app.processEvents();
        QDialog *own_dialog = nullptr;
        for (QDialog *dialog : heads_widget.findChildren<QDialog *>(QStringLiteral("projectsRevisionOpenDialog")))
            if (dialog->isVisible()) own_dialog = dialog;
        if (!require(own_dialog != nullptr
                         && own_dialog->findChild<QRadioButton *>(
                             QStringLiteral("projectsRevisionOpenEditBranch")) == nullptr,
                     "a personal revision offers a checkout edit instead of its HEAD")) return 1;
        own_dialog->reject();
        app.processEvents();

        // A database project can be deleted, after a confirmation that names
        // what goes and that its clones elsewhere stay.
        emit heads_graph->projectActivated(own_project);
        app.processEvents();
        QPushButton *delete_project = nullptr;
        for (QPushButton *button : heads_widget.findChildren<QPushButton *>(QStringLiteral("projectInspectorDeleteButton")))
            if (button->text() == QStringLiteral("Delete project…")) delete_project = button;
        if (!require(delete_project != nullptr && !delete_project->isHidden() && delete_project->isEnabled()
                         && delete_project->toolTip().contains(QStringLiteral("Permanently delete")),
                     "a database project cannot be deleted")) return 1;
        delete_project->click();
        app.processEvents();
        QMessageBox *confirm_delete = visibleMessage(heads_widget);
        if (!require(confirm_delete != nullptr
                         && confirm_delete->text().contains(QStringLiteral("'Shared name' from 'Own'"))
                         && confirm_delete->text().contains(QStringLiteral("cannot be undone"))
                         && confirm_delete->text().contains(QStringLiteral("'Team', 'Team 2' are not touched")),
                     "the project deletion confirmation does not say what goes and what stays")) return 1;
        confirm_delete->button(QMessageBox::Cancel)->click();
        app.processEvents();
        if (!require(!own.project_deleted, "a cancelled project deletion deleted the project")) return 1;
        delete_project->click();
        app.processEvents();
        confirm_delete = visibleMessage(heads_widget);
        if (!require(confirm_delete != nullptr, "the project deletion confirmation did not reopen")) return 1;
        confirm_delete->button(QMessageBox::Yes)->click();
        if (!require(waitUntil(app, [&] {
                return own.project_deleted && heads_graph->nodeTitle(own_tip).isEmpty()
                    && !heads_graph->nodeTitle(shared_tip).isEmpty();
            }), "a confirmed project deletion did not remove only that project")) return 1;
        if (!require(own.deleted_expected_name == QStringLiteral("Shared name"),
                     "the project deletion did not send the confirmed name")) return 1;
    }

    // Adding a connection that is already configured must explain why in a
    // dialog, keep the unsaved form open and store nothing. Before this check
    // the same repository at the same endpoint was silently selected instead.
    {
        FakeDvmdb alias;
        alias.repository_id = second.repository_id; // the same repository behind another URL
        if (!require(alias.start(), "repository alias fixture failed")) return 1;
        ProjectServerProfile configured = b;
        configured.wireguard_profile_id.clear();
        if (!require(saveProjectServerProfiles({configured}), "duplicate-connection setup failed")) return 1;
        ProjectsWidget duplicates;
        duplicates.resize(1200, 740);
        duplicates.show();
        QTreeWidget *explorer = duplicates.findChild<QTreeWidget *>(QStringLiteral("projectsTree"));
        if (!require(waitUntil(app, [&] {
                return explorer != nullptr && explorer->topLevelItemCount() == 1
                    && explorer->topLevelItem(0)->childCount() == 1;
            }), "duplicate-connection fixture did not load")) return 1;
        QPushButton *add = duplicates.findChild<QPushButton *>(
            QStringLiteral("projectsExplorerAddServerConnectionButton"));
        QWidget *overview_form = duplicates.findChild<QWidget *>(QStringLiteral("projectsDatabaseOverviewForm"));
        QLineEdit *name = overview_form != nullptr
            ? overview_form->findChild<QLineEdit *>(QStringLiteral("projectInspectorName")) : nullptr;
        QLineEdit *endpoint = duplicates.findChild<QLineEdit *>(QStringLiteral("projectServerEndpoint"));
        QComboBox *kind = duplicates.findChild<QComboBox *>(QStringLiteral("projectSourceKind"));
        QPushButton *create = duplicates.findChild<QPushButton *>(QStringLiteral("projectSourceCreateButton"));
        if (!require(add != nullptr && name != nullptr && endpoint != nullptr && kind != nullptr
                         && create != nullptr, "missing new-connection controls")) return 1;
        const auto attempt = [&](const QString &connection_name, const QString &url)
        {
            add->click();
            app.processEvents();
            name->setText(connection_name);
            endpoint->setText(url);
            create->click();
        };
        const auto storedEndpoints = []
        {
            QStringList endpoints;
            for (const ProjectServerProfile &profile : loadProjectServerProfiles())
                endpoints.append(profile.endpoint);
            return endpoints;
        };
        const QStringList original = storedEndpoints();

        // Same server, different spelling of its URL: rejected synchronously,
        // before any probe, naming the configured connection.
        const QStringList same_endpoint = {
            configured.endpoint,
            configured.endpoint + QStringLiteral("/"),
            configured.endpoint + QStringLiteral("/dvmdb/v1/"),
        };
        for (const QString &url : same_endpoint)
        {
            attempt(QStringLiteral("Second copy"), url);
            QMessageBox *message = visibleMessage(duplicates);
            if (!require(message != nullptr && message->icon() == QMessageBox::Warning
                             && message->windowTitle() == QStringLiteral("Database already configured")
                             && message->text().contains(QStringLiteral("'Source B'"))
                             && message->text().contains(url),
                         "duplicate endpoint was not rejected with a warning before probing")) return 1;
            message->accept();
            app.processEvents();
            if (!require(storedEndpoints() == original && kind->currentData().toString() == QStringLiteral("remote")
                             && !create->isHidden() && name->text() == QStringLiteral("Second copy")
                             && endpoint->text() == url,
                         "duplicate endpoint changed the stored connections or closed the form")) return 1;
        }

        // Same name (case-insensitive): rejected before the unreachable URL is probed.
        attempt(QStringLiteral("source b"), QStringLiteral("http://127.0.0.1:1"));
        QMessageBox *name_message = visibleMessage(duplicates);
        if (!require(name_message != nullptr && name_message->icon() == QMessageBox::Warning
                         && name_message->windowTitle() == QStringLiteral("Already exists")
                         && name_message->text().contains(QStringLiteral("'Source B'")),
                     "duplicate name was not rejected with a warning")) return 1;
        name_message->accept();
        app.processEvents();
        if (!require(storedEndpoints() == original && !create->isHidden(),
                     "duplicate name changed the stored connections or closed the form")) return 1;

        // The same repository UUIDv7 behind another URL is only known after the
        // probe; the user is asked whether to move the existing connection.
        attempt(QStringLiteral("Alias of B"), alias.endpoint());
        if (!require(visibleMessage(duplicates) == nullptr,
                     "a new URL was rejected before its repository was verified")) return 1;
        if (!require(waitUntil(app, [&] {
                QMessageBox *message = visibleMessage(duplicates);
                return message != nullptr && message->icon() == QMessageBox::Question
                    && message->text().contains(QStringLiteral("'Source B'"))
                    && message->text().contains(alias.endpoint());
            }), "same repository behind another URL was not reported")) return 1;
        visibleMessage(duplicates)->done(QMessageBox::No);
        app.processEvents();
        if (!require(storedEndpoints() == original && !create->isHidden(),
                     "declining to move the existing connection still changed it")) return 1;

        // A genuinely new connection is still created.
        attempt(QStringLiteral("Source C"), first.endpoint());
        if (!require(waitUntil(app, [&] { return loadProjectServerProfiles().size() == 2; }),
                     "a unique connection could not be created after rejected duplicates")) return 1;
        if (!require(visibleMessage(duplicates) == nullptr,
                     "creating a unique connection showed a message")) return 1;
    }

    std::puts("Multi-source explorer and health monitor tests passed");
    return 0;
}
