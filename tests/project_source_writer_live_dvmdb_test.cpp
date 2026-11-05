#include "projects/project_source_writer.h"
#include "projects/dvmdb_identity.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
struct Reply {
    int status = 0;
    QJsonObject body;
};

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QString id() { return DvmdbIdentity::newUuidV7(); }

bool await(QCoreApplication& app, const std::function<bool()>& condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 8000)
        app.processEvents(QEventLoop::AllEvents, 25);
    return condition();
}

Reply request(QNetworkAccessManager& manager, const QString& endpoint,
              const QByteArray& method, const QString& route, const QJsonObject& body = {}) {
    QNetworkRequest call(QUrl(endpoint + route));
    call.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    call.setTransferTimeout(6500);
    QNetworkReply* reply = method == "GET" ? manager.get(call) :
        manager.post(call, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { reply->abort(); });
    deadline.start(7000);
    if (!reply->isFinished()) loop.exec();
    const Reply result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                       QJsonDocument::fromJson(reply->readAll()).object()};
    reply->deleteLater();
    return result;
}

QString locateServer() {
    const QByteArray variable = qgetenv("AOWIS_DVMDB_SERVER_BIN");
    if (!variable.isEmpty()) return QString::fromLocal8Bit(variable);
    const QString guiBuild = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QStringLiteral("../../AOWIS-SERVER-DVMDB/build-linux-tests/aowis-server-dvmdb"),
        QStringLiteral("../../AOWIS-SERVER-DVMDB/build-linux-tests-debug/aowis-server-dvmdb"),
        QStringLiteral("../../AOWIS-SERVER-DVMDB/build-linux/aowis-server-dvmdb")};
    for (const QString& relative : candidates) {
        const QString candidate = QDir::cleanPath(guiBuild + '/' + relative);
        const QFileInfo file(candidate);
        if (file.isFile() && file.isExecutable()) return candidate;
    }
    return {};
}

void run(QCoreApplication& app, const QString& executable) {
    QTemporaryDir temp;
    check(temp.isValid(), "temporary DVMDB data root unavailable");
    QTcpServer reservation;
    check(reservation.listen(QHostAddress::LocalHost, 0), "cannot reserve test port");
    const quint16 port = reservation.serverPort();
    reservation.close();
    const QString endpoint = QStringLiteral("http://127.0.0.1:%1/dvmdb/v1").arg(port);
    QProcess process;
    const std::function<void()> start = [&] {
        process.setProgram(executable);
        process.setArguments({QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
            QStringLiteral("--port"), QString::number(port),
            QStringLiteral("--data-root"), temp.path()});
        process.start();
        check(process.waitForStarted(7000), "DVMDB process failed to start");
        QElapsedTimer timer;
        timer.start();
        bool ready = false;
        while (timer.elapsed() < 7000 && process.state() != QProcess::NotRunning) {
            QNetworkAccessManager health;
            if (request(health, endpoint, "GET", "/health").status == 200) {
                ready = true;
                break;
            }
            QThread::msleep(30);
        }
        check(ready, "DVMDB process did not become healthy");
    };
    const std::function<void()> stop = [&] {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(7000)) {
                process.kill();
                process.waitForFinished(3000);
                throw std::runtime_error("DVMDB process did not stop");
            }
        }
    };
    try {
        start();
        QNetworkAccessManager manager;
        const Reply tracking = request(manager, endpoint, "GET", "/repository/tracking");
        const QString repository = tracking.body.value("repositoryId").toString();
        check(tracking.status == 200 && DvmdbIdentity::isUuidV7(repository)
              && tracking.body.value("role").toString() == QStringLiteral("unconfigured"),
              "fresh DVMDB did not report unconfigured repository role");
        ProjectServerProfile profile;
        profile.id = repository;
        profile.connection_type = QStringLiteral("remote");
        profile.endpoint = endpoint;
        profile.auth_mode = QStringLiteral("None");
        ProjectSourceWriter writer;
        QString project;
        QString error;
        bool done = false;
        // A brand-new server must accept its first project without being
        // permanently configured as shared (or private) by the metadata writer.
        QString firstProject;
        writer.createProject(profile, QStringLiteral("Initial unconfigured project"),
                             [&](const QString& value, const QString& failure) {
            firstProject = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }) && error.isEmpty()
              && DvmdbIdentity::isUuidV7(firstProject),
              "GUI cannot create the first project on an unconfigured live DVMDB");
        const Reply afterCreate = request(manager, endpoint, "GET", "/repository/tracking");
        check(afterCreate.status == 200 && afterCreate.body.value("role").toString()
              == QStringLiteral("unconfigured")
              && request(manager, endpoint, "GET", "/projects/" + firstProject).status == 200,
              "first project write changed repository role or failed to persist");
        check(request(manager, endpoint, "POST", "/repository/role",
              QJsonObject{{"expectedRepositoryId", repository}, {"role", "private"}}).status == 200,
              "cannot configure live DVMDB as personal after creating first project");
        profile.personal = true;
        done = false;
        writer.createProject(profile, QStringLiteral("Integration deletion"),
                             [&](const QString& value, const QString& failure) {
            project = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }) && error.isEmpty() &&
              DvmdbIdentity::isUuidV7(project), "GUI writer could not create real project");
        QString doomed;
        done = false;
        writer.createUnbornBranch(profile, project, QStringLiteral("doomed"), {}, {},
                                  [&](const QString& value, const QString& failure) {
            doomed = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }) && error.isEmpty() &&
              DvmdbIdentity::isUuidV7(doomed), "GUI writer could not create real branch");
        QString surviving;
        done = false;
        writer.createUnbornBranch(profile, project, QStringLiteral("surviving"), {}, {},
                                  [&](const QString& value, const QString& failure) {
            surviving = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }) && error.isEmpty() &&
              DvmdbIdentity::isUuidV7(surviving), "GUI writer could not create second branch");
        const QString doomedDraft = id();
        const QString survivingDraft = id();
        for (const QPair<QString, QString>& item :
             {QPair<QString, QString>(doomed, doomedDraft),
              QPair<QString, QString>(surviving, survivingDraft)}) {
            check(request(manager, endpoint, "POST", "/working-copies",
                QJsonObject{{"id", item.second}, {"sessionId", id()},
                            {"projectId", project}, {"branchId", item.first},
                            {"baseRevision", QJsonValue(QJsonValue::Null)}}).status == 201,
                  "could not prepare real working copy");
        }
        done = false;
        writer.deleteBranch(profile, project, doomed, id(),
                            [&](const QString&, const QString& failure) {
            error = failure; done = true;
        });
        check(await(app, [&] { return done; }) && !error.isEmpty(),
              "GUI writer accepted stale expected HEAD");
        check(request(manager, endpoint, "GET", "/working-copies/" + doomedDraft).status == 200,
              "stale GUI confirmation discarded draft");
        done = false;
        QString deleted;
        writer.deleteBranch(profile, project, doomed, {},
                            [&](const QString& value, const QString& failure) {
            deleted = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }),
              "GUI writer deletion callback never completed");
        if (!error.isEmpty() || deleted != QStringLiteral("deleted")) {
            const Reply branchState = request(manager, endpoint, "GET",
                "/projects/" + project + "/branch-ids/" + doomed);
            const Reply draftState = request(manager, endpoint, "GET",
                "/working-copies/" + doomedDraft);
            throw std::runtime_error(QStringLiteral(
                "GUI writer could not confirm deletion through live DVMDB: "
                "server=%1; error='%2'; result='%3'; branch HTTP=%4; "
                "working-copy HTTP=%5; branch body=%6")
                .arg(executable, error, deleted)
                .arg(branchState.status).arg(draftState.status)
                .arg(QString::fromUtf8(QJsonDocument(branchState.body).toJson(QJsonDocument::Compact)))
                .toStdString());
        }
        const QString branchPath = "/projects/" + project + "/branch-ids/" + doomed;
        check(request(manager, endpoint, "GET", branchPath).status == 404 &&
              request(manager, endpoint, "GET", "/working-copies/" + doomedDraft).status == 404 &&
              request(manager, endpoint, "GET", "/working-copies/" + survivingDraft).status == 200,
              "live GUI deletion did not preserve the other draft");
        // Revision truncation is the second destructive writer path. A
        // committed root is created with real author metadata and its draft
        // must be removed as part of the confirmed first-revision truncation.
        const Reply committed = request(manager, endpoint, "POST", "/projects",
            QJsonObject{{"name", "Truncate integration"},
                {"author", QJsonObject{{"id", id()}, {"name", "GUI integration test"}}}});
        const QString committedProject = committed.body.value("id").toString();
        check(committed.status == 201 && DvmdbIdentity::isUuidV7(committedProject),
              "cannot create committed project for live revision truncation");
        const Reply graph = request(manager, endpoint, "GET",
            "/projects/" + committedProject + "/revision-graph?limit=5");
        const QJsonArray heads = graph.body.value("heads").toArray();
        check(graph.status == 200 && !heads.isEmpty(), "committed branch graph unavailable");
        const QJsonObject mainHead = heads.first().toObject();
        const QString committedBranch = mainHead.value("branchId").toString();
        const QString committedRevision = mainHead.value("revision").toString();
        check(DvmdbIdentity::isUuidV7(committedBranch) &&
              DvmdbIdentity::isUuidV7(committedRevision), "committed HEAD identity missing");
        const QString truncDraft = id();
        check(request(manager, endpoint, "POST", "/working-copies",
            QJsonObject{{"id", truncDraft}, {"sessionId", id()},
                        {"projectId", committedProject}, {"branchId", committedBranch},
                        {"baseRevision", committedRevision}}).status == 201,
              "cannot prepare real committed working copy");
        done = false;
        writer.truncateBranch(profile, committedProject, committedBranch,
                              committedRevision, committedRevision,
                              [&](const QString& value, const QString& failure) {
            deleted = value; error = failure; done = true;
        });
        check(await(app, [&] { return done; }),
              "GUI writer truncation callback never completed");
        if (!error.isEmpty() || deleted != QStringLiteral("deleted")) {
            throw std::runtime_error(QStringLiteral(
                "GUI writer could not truncate revision through live DVMDB: "
                "server=%1; error='%2'; result='%3'; project=%4; branch=%5; "
                "expectedHead=%6; firstRemoved=%7")
                .arg(executable, error, deleted, committedProject, committedBranch,
                     committedRevision, committedRevision)
                .toStdString());
        }
        check(request(manager, endpoint, "GET", "/working-copies/" + truncDraft).status == 404 &&
              request(manager, endpoint, "GET", "/projects/" + committedProject +
                  "/branch-ids/" + committedBranch).status == 404,
              "live GUI revision truncation did not delete branch and draft");
        stop();
        start();
        check(request(manager, endpoint, "GET", "/working-copies/" + truncDraft).status == 404 &&
              request(manager, endpoint, "GET", "/projects/" + committedProject +
                  "/branch-ids/" + committedBranch).status == 404,
              "live revision truncation was not durable after restart");
        check(request(manager, endpoint, "GET", branchPath).status == 404 &&
              request(manager, endpoint, "GET", "/working-copies/" + doomedDraft).status == 404 &&
              request(manager, endpoint, "GET", "/working-copies/" + survivingDraft).status == 200,
              "live deletion did not survive DVMDB restart");
        stop();
    } catch (...) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
        throw;
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString executable = locateServer();
    if (executable.isEmpty()) {
        std::fprintf(stderr, "SKIP: set AOWIS_DVMDB_SERVER_BIN to the built aowis-server-dvmdb executable\n");
        return 77;
    }
    try {
        run(app, executable);
        std::puts("GUI writer to live DVMDB destructive integration passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "GUI live DVMDB deletion test failed: %s\n", failure.what());
        return 1;
    }
}
