#ifndef STANDALONE_MAP_CASCADE_CONFIGURATION_H
#define STANDALONE_MAP_CASCADE_CONFIGURATION_H

#include <QString>
#include <QVector>

// Embedded MAP origin selection. Unlike the ordinary GUI map-server connection,
// these are upstreams used *by* the Standalone MAP core on a cache miss.
struct StandaloneMapUpstream
{
    QString name;
    QString url;
    QString api_key;
};

struct StandaloneMapCascadeConfiguration
{
    QVector<StandaloneMapUpstream> upstreams;
    bool allow_direct_fetch = true;
    bool poor_internet_mode = false;
    int jpeg_quality = 75;
    bool adaptive_compression = true;
};

bool validateStandaloneMapCascadeConfiguration(
    const StandaloneMapCascadeConfiguration &configuration, QString *error = nullptr);
// Invalid stored settings fail closed. The caller must NOT use the ordinary
// direct-provider defaults when this function returns false.
bool readStandaloneMapCascadeConfiguration(
    StandaloneMapCascadeConfiguration *configuration, QString *error = nullptr);
bool saveStandaloneMapCascadeConfiguration(
    const StandaloneMapCascadeConfiguration &configuration, QString *error = nullptr);

#endif // STANDALONE_MAP_CASCADE_CONFIGURATION_H
