#include "projects/model_codec_compatibility.h"
#include "tabs/tab_projects_widget.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"
#include "services/http_health_monitor.h"
#include "widgets/group_box_collapsible.h"

#include <QApplication>
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
#include <QTreeWidget>
#include <QTimer>

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

struct FakeDvmdb
{
    QTcpServer server;
    bool valid = true;
    bool graph_valid = true;
    bool catalogue_valid = true;
    int catalogue_requests = 0;
    int tracking_requests = 0;
    QJsonObject created_project;
    QString repository_id = QStringLiteral("018f84c0-0000-7000-8000-00000000d001");
    const QString hash = QStringLiteral("018f84c0-0000-7000-8000-00000000b001");
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
                                {QStringLiteral("role"), QStringLiteral("shared")},
                                {QStringLiteral("repositoryId"), this->repository_id},
                                {QStringLiteral("modelCodecFingerprint"), ModelCodecCompatibility::fingerprint()}
                            }).toJson();
                        }
                        else if (path == "/dvmdb/v1/projects" && buffer->startsWith("POST "))
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
                            const QJsonObject project = QJsonDocument::fromJson(body).object();
                            if (length != body.size() || expected_id != this->repository_id.toUtf8()
                                || !DvmdbIdentity::isUuidV7(project.value(QStringLiteral("id")).toString()))
                                status = "400 Bad Request";
                            else
                            {
                                this->created_project = project;
                                data = QJsonDocument(project).toJson();
                                status = "201 Created";
                            }
                        }
                        else if (path == "/dvmdb/v1/projects")
                        {
                            ++this->catalogue_requests;
                            if (this->catalogue_valid)
                            {
                                QJsonArray projects{QJsonObject{
                                    {QStringLiteral("id"), this->project_id},
                                    {QStringLiteral("name"), QStringLiteral("Shared name")}}};
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
                                const QJsonObject response{
                                    {QStringLiteral("revisions"), QJsonArray{revision}},
                                    {QStringLiteral("heads"), QJsonArray{QJsonObject{
                                        {QStringLiteral("branch"), QStringLiteral("main")},
                                        {QStringLiteral("branchId"), this->branch_id},
                                        {QStringLiteral("revision"), this->hash},
                                        {QStringLiteral("recordHash"), this->digest}}}},
                                    {QStringLiteral("complete"), true},
                                    {QStringLiteral("nextCursor"), QJsonValue(QJsonValue::Null)}};
                                data = QJsonDocument(response).toJson();
                            }
                        }
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
    QTemporaryFile profile_file(config.filePath(QStringLiteral("peer-XXXXXX.conf")));
    if (argc == 2)
    {
        peer.start(QString::fromLocal8Bit(argv[1]), {QString::number(first.server.serverPort())});
        if (!require(peer.waitForStarted() && waitUntil(app, [&] { return peer.canReadLine(); }),
                     "WireGuard peer did not start")) return 1;
        const QJsonObject ready = QJsonDocument::fromJson(peer.readLine()).object();
        const QByteArray key = ready.value(QStringLiteral("client_private_key")).toString().toLatin1();
        const QByteArray public_key = ready.value(QStringLiteral("server_public_key")).toString().toLatin1();
        const QByteArray endpoint = ready.value(QStringLiteral("endpoint")).toString().toLatin1();
        const QByteArray config_text = "[Interface]\nAddress=10.173.91.2/32\nPrivateKey=" + key
            + "\n[Peer]\nPublicKey=" + public_key + "\nEndpoint=" + endpoint + "\nAllowedIPs=10.173.91.1/32\n";
        if (!require(profile_file.open() && profile_file.write(config_text) == config_text.size()
                     && profile_file.flush(), "could not write temporary WireGuard profile")) return 1;
        profile_file.close();
        a.endpoint = QStringLiteral("http://10.173.91.1:18080");
        a.wireguard_profile_path = profile_file.fileName();
    }
    if (!require(saveProjectServerProfiles({a, b}), "could not store multiple sources")) return 1;

    ProjectsWidget projects;
    projects.resize(1200, 740);
    projects.show();
    QTreeWidget *sources = projects.findChild<QTreeWidget *>(QStringLiteral("serversTree"));
    QTreeWidget *tree = projects.findChild<QTreeWidget *>(QStringLiteral("projectsTree"));
    ProjectRevisionGraph *graph = projects.findChild<ProjectRevisionGraph *>(
        QStringLiteral("projectRevisionGraph"));
    if (!require(sources != nullptr && tree != nullptr && graph != nullptr,
                 "missing multi-source components")) return 1;
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
    if (argc == 2)
    {
        // A second tunnel would have to reread the profile. Removing the
        // temporary file proves that Verify and Refresh reuse the active one.
        if (!require(profile_file.remove(), "could not remove temporary profile")) return 1;
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
    first.graph_valid = false;
    refresh->click();
    if (!require(waitUntil(app, [&] {
            return health_label->text() == QStringLiteral("Available · catalogue error")
                && source_status->text().contains(QStringLiteral("Invalid DVMDB revision graph page"));
        }), "catalogue failure incorrectly marked the responding server unavailable")) return 1;
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
    // Branch UUIDs are independently selectable reference cards, not revision
    // duplicates. Even when two databases have identical revision content,
    // each source's HEAD must point to its own revision card.
    const QString first_branch = QStringLiteral("group:branch:") + first.repository_id + QStringLiteral("|") + first.project_id
        + QStringLiteral(":") + first.branch_id;
    const QString second_branch = QStringLiteral("group:branch:") + second.repository_id + QStringLiteral("|") + second.project_id
        + QStringLiteral(":") + second.branch_id;
    if (!require(!graph->nodeViewportPosition(first_branch).isNull()
                 && !graph->nodeViewportPosition(second_branch).isNull()
                 && graph->nodeViewportPosition(first_branch).y()
                     > graph->revisionViewportPosition(first_graph_id).y()
                 && graph->nodeViewportPosition(second_branch).y()
                     > graph->revisionViewportPosition(second_graph_id).y(),
                 "distinct branch reference cards are missing below their HEAD revisions")) return 1;
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
    if (!require(tree->topLevelItemCount() == 1
                 && loadProjectServerProfiles().at(1).visible == false
                 && graph->revisionViewportPosition(second_graph_id).isNull(),
                 "unchecking a source did not persistently filter navigator and graph")) return 1;
    if (!require(graph->nodeViewportPosition(QStringLiteral("group:server:") + second.repository_id).isNull(),
                 "hidden database still has a graph header")) return 1;
    sources->topLevelItem(1)->setCheckState(0, Qt::Checked);
    app.processEvents();
    if (!require(tree->topLevelItemCount() == 2 && loadProjectServerProfiles().at(1).visible,
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

    std::puts("Multi-source explorer and health monitor tests passed");
    return 0;
}
