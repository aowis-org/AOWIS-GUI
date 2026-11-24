// Duplicate detection for new DVMDB connections: names, local data
// directories, remote endpoints (with normalization and tunnel identity) and
// verified repository identities, in that priority order.
#include "config/project_server_duplicates.h"
#include "projects/dvmdb_identity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace
{
int failures = 0;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        ++failures;
    }
}

ProjectServerProfile remote(const QString &name, const QString &endpoint)
{
    ProjectServerProfile profile;
    profile.id = DvmdbIdentity::newUuidV7();
    profile.name = name;
    profile.endpoint = endpoint;
    profile.connection_type = QStringLiteral("remote");
    profile.auth_mode = QStringLiteral("None");
    return profile;
}

ProjectServerProfile local(const QString &name, const QString &path)
{
    ProjectServerProfile profile;
    profile.id = DvmdbIdentity::newUuidV7();
    profile.name = name;
    profile.connection_type = QStringLiteral("local");
    profile.local_path = path;
    profile.auth_mode = QStringLiteral("None");
    return profile;
}

// A candidate as saveNewSource() builds it before verification.
ProjectServerProfile pendingRemote(const QString &name, const QString &endpoint)
{
    ProjectServerProfile profile = remote(name, endpoint);
    profile.pending = true;
    return profile;
}

bool is(const ProjectServerDuplicate &duplicate, ProjectServerDuplicateKind kind, qsizetype index)
{
    return duplicate.kind == kind && duplicate.index == index && static_cast<bool>(duplicate);
}

void testNormalizedEndpoint()
{
    const QString canonical = normalizedProjectServerEndpoint(QStringLiteral("http://db.example:8755"));
    require(canonical == QStringLiteral("http://db.example:8755"), "plain endpoint is not canonical");
    const QStringList same = {
        QStringLiteral("http://db.example:8755/"),
        QStringLiteral("http://db.example:8755///"),
        QStringLiteral("  http://db.example:8755  "),
        QStringLiteral("HTTP://DB.EXAMPLE:8755"),
        QStringLiteral("http://db.example:8755/dvmdb/v1"),
        QStringLiteral("http://db.example:8755/dvmdb/v1/"),
    };
    for (const QString &variant : same)
        require(normalizedProjectServerEndpoint(variant) == canonical,
                qPrintable(QStringLiteral("endpoint variant not normalized: ") + variant));

    require(normalizedProjectServerEndpoint(QStringLiteral("http://db.example"))
                == normalizedProjectServerEndpoint(QStringLiteral("http://db.example:80/")),
            "implicit and explicit default HTTP port differ");
    require(normalizedProjectServerEndpoint(QStringLiteral("https://db.example"))
                == normalizedProjectServerEndpoint(QStringLiteral("https://db.example:443")),
            "implicit and explicit default HTTPS port differ");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://db.example:8755"))
                != normalizedProjectServerEndpoint(QStringLiteral("http://db.example:8756")),
            "different ports compare equal");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://db.example"))
                != normalizedProjectServerEndpoint(QStringLiteral("https://db.example")),
            "http and https compare equal");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://db.example:80"))
                != normalizedProjectServerEndpoint(QStringLiteral("https://db.example:80")),
            "scheme is ignored when the port is explicit");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/a"))
                != normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/b")),
            "different reverse-proxy prefixes compare equal");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/a/dvmdb/v1"))
                == normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/a")),
            "API suffix under a proxy prefix is not stripped");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/A"))
                != normalizedProjectServerEndpoint(QStringLiteral("http://proxy.example/a")),
            "URL paths must stay case-sensitive");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://[::1]:8755/"))
                == normalizedProjectServerEndpoint(QStringLiteral("http://[0:0:0:0:0:0:0:1]:8755")),
            "IPv6 host forms are not normalized");
    require(normalizedProjectServerEndpoint(QStringLiteral("http://[::1]:8755")).contains(QStringLiteral("[::1]")),
            "IPv6 host lost its brackets");
    require(normalizedProjectServerEndpoint(QStringLiteral("  not a url ")) == QStringLiteral("not a url"),
            "unparseable endpoint is not returned trimmed");
    require(normalizedProjectServerEndpoint(QStringLiteral("ftp://db.example")) == QStringLiteral("ftp://db.example"),
            "non-HTTP endpoint must not be rewritten");
    require(normalizedProjectServerEndpoint(QString()).isEmpty(), "empty endpoint is not empty");
}

void testNames()
{
    const QList<ProjectServerProfile> existing = {
        remote(QStringLiteral("Office"), QStringLiteral("http://office.example:8755")),
        remote(QStringLiteral("Plant"), QStringLiteral("http://plant.example:8755")),
    };
    require(!findProjectServerDuplicate({}, pendingRemote(QStringLiteral("Office"),
                                                          QStringLiteral("http://x.example"))),
            "an empty list reported a duplicate");
    require(!findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Lab"),
                                                                QStringLiteral("http://lab.example"))),
            "a unique connection reported a duplicate");
    require(is(findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("plant"),
                                                                  QStringLiteral("http://new.example"))),
               ProjectServerDuplicateKind::Name, 1),
            "names must compare case-insensitively");
    require(is(findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("  Office "),
                                                                  QStringLiteral("http://new.example"))),
               ProjectServerDuplicateKind::Name, 0),
            "names must compare trimmed");
    require(!findProjectServerDuplicate(existing, pendingRemote(QString(), QStringLiteral("http://new.example"))),
            "an empty name matched");
    // The name wins over every other reason so the dialog names the field to change first.
    ProjectServerProfile both = pendingRemote(QStringLiteral("Office"), QStringLiteral("http://plant.example:8755"));
    require(is(findProjectServerDuplicate(existing, both), ProjectServerDuplicateKind::Name, 0),
            "name duplicate is not reported before endpoint duplicate");
}

void testEndpoints()
{
    ProjectServerProfile pending_existing = remote(QStringLiteral("Offline"), QStringLiteral("http://offline.example:8755"));
    pending_existing.pending = true;
    ProjectServerProfile tunnelled = remote(QStringLiteral("Tunnel"), QStringLiteral("http://10.0.0.1:8755"));
    tunnelled.wireguard_profile_id = QStringLiteral("018f84c0-0000-7000-8000-0000000000f1");
    const QList<ProjectServerProfile> existing = {
        local(QStringLiteral("Local"), QStringLiteral("/srv/dvmdb")),
        remote(QStringLiteral("Office"), QStringLiteral("http://office.example:8755/")),
        pending_existing,
        tunnelled,
    };
    require(is(findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Office 2"),
                                                                  QStringLiteral("http://office.example:8755"))),
               ProjectServerDuplicateKind::Endpoint, 1),
            "same endpoint without trailing slash was not detected");
    require(is(findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Office 3"),
                                                                  QStringLiteral("http://OFFICE.example:8755/dvmdb/v1"))),
               ProjectServerDuplicateKind::Endpoint, 1),
            "same endpoint with API suffix and upper-case host was not detected");
    require(is(findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Offline 2"),
                                                                  QStringLiteral("http://offline.example:8755"))),
               ProjectServerDuplicateKind::Endpoint, 2),
            "an unverified pending connection with the same endpoint was not detected");
    require(!findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Office 4"),
                                                                QStringLiteral("http://office.example:8756"))),
            "a different port was treated as a duplicate");

    // The tunnel is part of the location: the same private address through
    // another tunnel (or directly) can be another server.
    ProjectServerProfile same_tunnel = pendingRemote(QStringLiteral("Tunnel 2"), QStringLiteral("http://10.0.0.1:8755/"));
    same_tunnel.wireguard_profile_id = tunnelled.wireguard_profile_id;
    require(is(findProjectServerDuplicate(existing, same_tunnel), ProjectServerDuplicateKind::Endpoint, 3),
            "same endpoint through the same tunnel was not detected");
    ProjectServerProfile other_tunnel = same_tunnel;
    other_tunnel.wireguard_profile_id = QStringLiteral("018f84c0-0000-7000-8000-0000000000f2");
    require(!findProjectServerDuplicate(existing, other_tunnel),
            "same endpoint through another tunnel was treated as a duplicate");
    ProjectServerProfile direct = same_tunnel;
    direct.wireguard_profile_id.clear();
    require(!findProjectServerDuplicate(existing, direct),
            "direct connection to a tunnelled address was treated as a duplicate");

    // A remote candidate never matches a local profile, and empty endpoints never match.
    ProjectServerProfile empty_endpoint = remote(QStringLiteral("Empty"), QString());
    const QList<ProjectServerProfile> with_empty = {empty_endpoint};
    require(!findProjectServerDuplicate(with_empty, pendingRemote(QStringLiteral("Empty 2"), QStringLiteral(" "))),
            "empty endpoints matched each other");
    require(!findProjectServerDuplicate(existing, pendingRemote(QStringLiteral("Srv"), QStringLiteral("/srv/dvmdb"))),
            "a remote candidate matched a local data directory");
}

void testLocalPaths()
{
    QTemporaryDir root;
    require(root.isValid(), "temporary directory unavailable");
    const QString data = QDir(root.path()).filePath(QStringLiteral("data"));
    require(QDir().mkpath(data), "could not create data directory");
    const QString link = QDir(root.path()).filePath(QStringLiteral("link"));
    require(QFile::link(data, link), "could not create symbolic link");
    const QString missing = QDir(root.path()).filePath(QStringLiteral("missing"));

    const QList<ProjectServerProfile> existing = {
        remote(QStringLiteral("Office"), QStringLiteral("http://office.example")),
        local(QStringLiteral("Data"), data),
        local(QStringLiteral("Missing"), missing),
    };
    ProjectServerProfile candidate = local(QStringLiteral("Data 2"), data + QStringLiteral("/"));
    candidate.pending = true;
    require(is(findProjectServerDuplicate(existing, candidate), ProjectServerDuplicateKind::LocalPath, 1),
            "same data directory with trailing slash was not detected");
    candidate.local_path = QDir(data).filePath(QStringLiteral("../data/./"));
    require(is(findProjectServerDuplicate(existing, candidate), ProjectServerDuplicateKind::LocalPath, 1),
            "same data directory with dot segments was not detected");
    candidate.local_path = link;
    require(is(findProjectServerDuplicate(existing, candidate), ProjectServerDuplicateKind::LocalPath, 1),
            "same data directory through a symbolic link was not detected");
    candidate.local_path = missing + QStringLiteral("/.");
    require(is(findProjectServerDuplicate(existing, candidate), ProjectServerDuplicateKind::LocalPath, 2),
            "a not-yet-created data directory was not compared by its cleaned path");
    candidate.local_path = QDir(root.path()).filePath(QStringLiteral("other"));
    require(!findProjectServerDuplicate(existing, candidate), "a different data directory was treated as a duplicate");
    ProjectServerProfile no_path = local(QStringLiteral("No path"), QString());
    require(!findProjectServerDuplicate({local(QStringLiteral("Also no path"), QString())}, no_path),
            "empty data directories matched each other");

    require(normalizedProjectServerLocalPath(link) == normalizedProjectServerLocalPath(data),
            "symbolic link did not resolve to the canonical directory");
    require(normalizedProjectServerLocalPath(missing + QStringLiteral("//x/..")) == missing,
            "missing directory was not cleaned");
}

void testRepositories()
{
    ProjectServerProfile office = remote(QStringLiteral("Office"), QStringLiteral("http://office.example:8755"));
    ProjectServerProfile offline = remote(QStringLiteral("Offline"), QStringLiteral("http://offline.example:8755"));
    offline.pending = true;
    ProjectServerProfile embedded = local(QStringLiteral("Embedded"), QStringLiteral("/srv/dvmdb"));
    const QList<ProjectServerProfile> existing = {office, offline, embedded};

    // The same repository reached through another host name.
    ProjectServerProfile verified = remote(QStringLiteral("Office by IP"), QStringLiteral("http://192.0.2.10:8755"));
    verified.id = office.id;
    require(is(findProjectServerDuplicate(existing, verified), ProjectServerDuplicateKind::Repository, 0),
            "a verified repository UUID that is already configured was not detected");
    verified.pending = true;
    require(!findProjectServerDuplicate(existing, verified),
            "a pending candidate's temporary id was treated as a repository identity");

    // A pending connection's temporary id is never a repository identity.
    ProjectServerProfile against_pending = remote(QStringLiteral("Offline by IP"), QStringLiteral("http://192.0.2.11"));
    against_pending.id = offline.id;
    require(!findProjectServerDuplicate(existing, against_pending),
            "an existing pending connection's temporary id matched a verified repository");

    // Remote and local connections to one repository are duplicates too.
    ProjectServerProfile remote_to_embedded = remote(QStringLiteral("Served"), QStringLiteral("http://192.0.2.12"));
    remote_to_embedded.id = embedded.id;
    require(is(findProjectServerDuplicate(existing, remote_to_embedded), ProjectServerDuplicateKind::Repository, 2),
            "a remote connection to an embedded repository was not detected");
    ProjectServerProfile local_to_remote = local(QStringLiteral("Copy"), QStringLiteral("/srv/other"));
    local_to_remote.id = office.id;
    require(is(findProjectServerDuplicate(existing, local_to_remote), ProjectServerDuplicateKind::Repository, 0),
            "a local connection to a configured remote repository was not detected");

    // A location match is reported before the repository match.
    ProjectServerProfile same_everything = remote(QStringLiteral("Office again"), QStringLiteral("http://office.example:8755/"));
    same_everything.id = office.id;
    require(is(findProjectServerDuplicate(existing, same_everything), ProjectServerDuplicateKind::Endpoint, 0),
            "endpoint duplicate is not reported before repository duplicate");
    // Same URL, other tunnel, same repository: only the repository matches.
    ProjectServerProfile moved_tunnel = same_everything;
    moved_tunnel.wireguard_profile_id = QStringLiteral("018f84c0-0000-7000-8000-0000000000f3");
    require(is(findProjectServerDuplicate(existing, moved_tunnel), ProjectServerDuplicateKind::Repository, 0),
            "same repository through another tunnel was not detected by identity");

    ProjectServerProfile no_id = remote(QStringLiteral("No id"), QStringLiteral("http://192.0.2.13"));
    no_id.id.clear();
    ProjectServerProfile existing_no_id = remote(QStringLiteral("Existing no id"), QStringLiteral("http://192.0.2.14"));
    existing_no_id.id.clear();
    require(!findProjectServerDuplicate({existing_no_id}, no_id), "empty repository ids matched each other");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testNormalizedEndpoint();
    testNames();
    testEndpoints();
    testLocalPaths();
    testRepositories();
    if (failures != 0)
    {
        std::fprintf(stderr, "%d project server duplicate check(s) failed\n", failures);
        return 1;
    }
    std::puts("Project server duplicate tests passed");
    return 0;
}
