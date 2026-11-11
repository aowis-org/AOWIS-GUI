#include "config/standalone_map_cascade_configuration.h"
#include "config/gui_preferences.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary config directory")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QCoreApplication application(argc, argv);

    StandaloneMapCascadeConfiguration loaded;
    QString error;
    if (!require(readStandaloneMapCascadeConfiguration(&loaded, &error), "read default")) return 1;
    if (!require(loaded.upstreams.isEmpty() && loaded.allow_direct_fetch &&
                 !loaded.poor_internet_mode && loaded.jpeg_quality == 75 &&
                 loaded.adaptive_compression, "unchanged no-upstream defaults")) return 1;

    StandaloneMapCascadeConfiguration configured;
    configured.upstreams.append({QStringLiteral("primary"),
                                 QStringLiteral("https://primary.example.org:8443"),
                                 QStringLiteral("read-secret-1")});
    configured.upstreams.append({QStringLiteral("backup"),
                                 QStringLiteral("http://backup.example.org:8080/"),
                                 QStringLiteral("read-secret-2")});
    configured.allow_direct_fetch = false;
    configured.poor_internet_mode = true;
    configured.adaptive_compression = false;
    configured.jpeg_quality = 62;
    if (!require(saveStandaloneMapCascadeConfiguration(configured, &error), "save valid settings")) return 1;
    GuiPreferences::reload();
    if (!require(readStandaloneMapCascadeConfiguration(&loaded, &error), "reload settings")) return 1;
    if (!require(loaded.upstreams.size() == 2 &&
                 loaded.upstreams.at(0).name == QStringLiteral("primary") &&
                 loaded.upstreams.at(1).name == QStringLiteral("backup") &&
                 loaded.upstreams.at(0).api_key == QStringLiteral("read-secret-1") &&
                 loaded.upstreams.at(1).api_key == QStringLiteral("read-secret-2") &&
                 !loaded.allow_direct_fetch && loaded.poor_internet_mode &&
                 !loaded.adaptive_compression && loaded.jpeg_quality == 62,
                 "ordered upstreams, keys and options persist")) return 1;

    configured.upstreams[1].name = QStringLiteral("primary");
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "duplicate name rejected")) return 1;
    configured.upstreams[1].name = QStringLiteral("backup");
    configured.upstreams[1].url = QStringLiteral("https://primary.example.org:8443/");
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "duplicate URL rejected")) return 1;
    configured.upstreams[1].url = QStringLiteral("https://other.example.org/tiles");
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "URL subpath rejected")) return 1;
    configured.upstreams[1].url = QStringLiteral("https://user:secret@other.example.org");
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "URL credentials rejected")) return 1;
    configured.upstreams[1].url = QStringLiteral("file:///etc/passwd");
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "non-HTTP URL rejected")) return 1;
    configured.upstreams[1].url = QStringLiteral("http://backup.example.org:8080");
    configured.jpeg_quality = 101;
    if (!require(!saveStandaloneMapCascadeConfiguration(configured, &error),
                 "invalid JPEG quality rejected")) return 1;
    GuiPreferences::reload();
    if (!require(readStandaloneMapCascadeConfiguration(&loaded, &error) &&
                 loaded.jpeg_quality == 62 && loaded.upstreams.at(1).name == QStringLiteral("backup"),
                 "invalid saves cannot overwrite good state")) return 1;

    // Corrupt persisted state must not silently restore the direct-provider
    // default. The caller rejects it and disables all remote fetching.
    if (!require(GuiPreferences::update(QStringLiteral("embedded_map_cascade"),
                  QJsonObject{{QStringLiteral("upstreams"), QJsonArray{
                      QJsonObject{{QStringLiteral("name"), QStringLiteral("bad")},
                                  {QStringLiteral("url"), QStringLiteral("ftp://invalid.example")},
                                  {QStringLiteral("api_key"), QStringLiteral("")}}}}}),
                 "inject malformed persisted settings")) return 1;
    GuiPreferences::reload();
    if (!require(!readStandaloneMapCascadeConfiguration(&loaded, &error),
                 "malformed persisted settings rejected")) return 1;
    if (!require(loaded.upstreams.isEmpty(), "malformed settings not partially applied")) return 1;
    return 0;
}
