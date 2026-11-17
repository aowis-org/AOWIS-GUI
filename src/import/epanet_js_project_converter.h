#ifndef EPANET_JS_PROJECT_CONVERTER_H
#define EPANET_JS_PROJECT_CONVERTER_H

#include "import/epanet_js_project_reader.h"

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>
#include <aowis/model/hydraulic/hydraulic_simulation_options.h>

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <optional>

enum class EpanetJsConversionDiagnosticSeverity
{
    Warning,
    Error
};

struct EpanetJsConversionDiagnostic
{
    EpanetJsConversionDiagnosticSeverity severity = EpanetJsConversionDiagnosticSeverity::Error;
    QString code;
    QString message;
    QString table_name;
    std::optional<qint64> source_id;
};

class EpanetJsProjectIdMap
{
public:
    EpanetJsProjectIdMap() = default;
    explicit EpanetJsProjectIdMap(const QUuid &project_uuid);

    QUuid projectUuid() const;
    bool registerId(const QString &table_name, qint64 source_id, QString *error = nullptr);
    bool contains(const QString &table_name, qint64 source_id) const;
    QUuid uuidFor(const QString &table_name, qint64 source_id) const;
    qsizetype size() const;

    QList<QString> nodeTablesForId(qint64 source_id) const;
    QList<QString> linkTablesForId(qint64 source_id) const;
    QUuid nodeUuid(qint64 source_id) const;
    QUuid linkUuid(qint64 source_id) const;

private:
    QUuid project_uuid;
    QMap<QString, QMap<qint64, QUuid>> uuids_by_table;
};

struct EpanetJsProjectConversionResult
{
    bool success = false;
    NetworkHydraulicDistribution network;
    QList<WaterQualitySolverOptions> quality_runs;
    QJsonObject source_display_units; // Resolved epanet-js quantities; not canonical data.
    EpanetJsProjectIdMap id_map;
    QList<EpanetJsConversionDiagnostic> diagnostics;

    bool hasErrors() const;
    QString errorSummary() const;
};

class EpanetJsProjectConverter
{
public:
    static EpanetJsProjectConversionResult convert(const EpanetJsProjectSnapshot &project);
};

#endif // EPANET_JS_PROJECT_CONVERTER_H
