#include "tabs/tab_projects_widget.h"
#include "config/project_server_profile_storage.h"
#include "projects/dvmdb_identity.h"
#include "services/http_health_monitor.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QStackedWidget>
#include <QTcpServer>
#include <QPushButton>
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
                    QByteArray data = R"({"broken":true})";
                    if (this->valid)
                    {
                        if (path == "/dvmdb/v1/repository")
                            data = QJsonDocument(QJsonObject{
                                {QStringLiteral("repositoryId"), this->repository_id}}).toJson();
                        else if (path == "/dvmdb/v1/projects")
                            data = QJsonDocument(QJsonObject{
                                {QStringLiteral("projects"), QJsonArray{QJsonObject{
                                    {QStringLiteral("id"), this->project_id},
                                    {QStringLiteral("name"), QStringLiteral("Shared name")}}}}}).toJson();
                        else if (path == QStringLiteral("/dvmdb/v1/projects/%1/revision-graph?limit=256")
                                     .arg(this->project_id).toUtf8())
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
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "
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
        }), "database catalogues did not load independently at startup")) return 1;
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
    sources->setCurrentItem(sources->topLevelItem(1));
    app.processEvents();
    if (!require(inspectorHasIdentity(second.repository_id),
                 "second database UUID was not shown separately")) return 1;
    tree->setCurrentItem(tree->topLevelItem(1)->child(0));
    app.processEvents();
    // Project and Branch inspectors each own an Edit network action. Query the
    // selected inspector, not the first identically named action in the tree.
    QStackedWidget *action_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorActionsStack"));
    QWidget *active_actions = action_stack != nullptr ? action_stack->currentWidget() : nullptr;
    QPushButton *edit_network = active_actions != nullptr
        ? active_actions->findChild<QPushButton *>(QStringLiteral("projectsInspectorEditNetwork"))
        : nullptr;
    if (!require(active_actions != nullptr
                     && active_actions->objectName() == QStringLiteral("projectsProjectInspector")
                     && edit_network != nullptr && edit_network->isVisible()
                     && edit_network->isEnabled(),
                 "database project does not expose its hydraulic editor action")) return 1;
    QStackedWidget *overview_stack = projects.findChild<QStackedWidget *>(
        QStringLiteral("projectsInspectorOverviewStack"));
    QLabel *metadata = overview_stack != nullptr && overview_stack->currentWidget() != nullptr
        ? overview_stack->currentWidget()->findChild<QLabel *>(
            QStringLiteral("projectInspectorMetadata")) : nullptr;
    if (!require(metadata != nullptr
                 && metadata->text().contains(QStringLiteral("Repository UUIDv7: ") + second.repository_id),
                 "project inspector lost the repository-qualified project identity")) return 1;
    const QString first_graph_id = first.repository_id + QStringLiteral("|") + first.project_id + QStringLiteral("|") + first.hash;
    const QString second_graph_id = second.repository_id + QStringLiteral("|") + second.project_id + QStringLiteral("|") + second.hash;
    graph->fitGraph();
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
    HttpHealthMonitor monitor;
    HttpHealthMonitor::State status = HttpHealthMonitor::State::Checking;
    monitor.start(QUrl(first.endpoint() + QStringLiteral("/dvmdb/v1/projects")),
        [](const QByteArray &body)
        {
            const QJsonDocument json = QJsonDocument::fromJson(body);
            return json.isObject() && json.object().value(QStringLiteral("projects")).isArray();
        }, [&](HttpHealthMonitor::State value, const QString &) { status = value; }, {}, 30000);
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Healthy; }),
                 "health probe did not validate a good service response")) return 1;
    first.valid = false;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Degraded; }),
                 "health probe did not degrade after invalid response")) return 1;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Offline; }),
                 "health probe did not declare offline after repeated failures")) return 1;
    first.valid = true;
    monitor.poll();
    if (!require(waitUntil(app, [&] { return status == HttpHealthMonitor::State::Healthy; }),
                 "health probe did not recover after service restoration")) return 1;
    monitor.stop();

    // A saved offline endpoint begins with a temporary UUIDv7 and becomes the
    // authoritative repository UUID after its first successful catalogue read.
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
