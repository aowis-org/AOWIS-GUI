// Project units: the open network's stored presentation units layered over
// the local unit profile (config/unit_profile_manager.h). Headless; the
// draft writer is a test double, so no HydraulicData or DVMDB is involved.
#include "config/unit_profile_manager.h"
#include "config/gui_preferences.h"
#include "unit_test_check.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>

#include <optional>

namespace {
int profileIndex(const QString &id)
{
    const QVector<UnitProfileManager::Profile> &profiles = UnitProfileManager::instance().profiles();
    for (int index = 0; index < profiles.size(); ++index)
        if (profiles.at(index).id == id) return index;
    return -1;
}

bool canonicallySorted(const NetworkPresentationUnits &units)
{
    for (qsizetype index = 1; index < units.units.size(); ++index)
        if (QString::compare(units.units.at(index - 1).quantity, units.units.at(index).quantity,
                             Qt::CaseSensitive) >= 0)
            return false;
    return true;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    // Isolates the GUI's AOWIS-CONFIG store; must precede QCoreApplication.
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QCoreApplication application(argc, argv);

    UnitProfileManager &manager = UnitProfileManager::instance();
    const QString flow = QStringLiteral("volumetric_flow_rate");
    const QString pressure = QStringLiteral("pressure_head");
    const QString local_flow = manager.effectiveUnits().value(flow).toString();
    const QString local_pressure = manager.effectiveUnits().value(pressure).toString();
    CHECK(manager.displaySource() == UnitProfileManager::DisplaySource::Project);
    CHECK(!manager.projectUnitsInEffect());
    CHECK(!local_flow.isEmpty() && local_flow != QStringLiteral("US gal/min"));

    // A network with partial project units: listed quantities apply, the rest
    // fall back to the local profile. Read-only: nothing can be written.
    NetworkPresentationUnits stored;
    stored.name = QStringLiteral("Utility");
    stored.units.append(NetworkPresentationUnit{flow, QStringLiteral("US gal/min")});
    manager.setNetworkProjectUnits(stored, false);
    CHECK(manager.projectUnitsInEffect());
    CHECK(manager.projectUnits().name == QStringLiteral("Utility"));
    CHECK(manager.effectiveUnits().value(flow).toString() == QStringLiteral("US gal/min"));
    CHECK(manager.effectiveUnits().value(pressure).toString() == local_pressure);
    CHECK(manager.presentation().unit(flow) == QStringLiteral("US gal/min"));
    CHECK(!manager.storeProfileInProject(0));
    CHECK(!manager.removeProjectUnits());

    // A choice the Model would reject never reaches the presentation.
    NetworkPresentationUnits invalid = stored;
    invalid.units.append(NetworkPresentationUnit{pressure, QStringLiteral("parsec")});
    manager.setNetworkProjectUnits(invalid, false);
    CHECK(manager.effectiveUnits().value(pressure).toString() == local_pressure);

    // Editable draft: storing a profile writes a complete, canonical copy.
    std::optional<std::optional<NetworkPresentationUnits>> written;
    manager.setProjectUnitsWriter([&written](const std::optional<NetworkPresentationUnits> &units) {
        written = units;
        return true;
    });
    manager.setNetworkProjectUnits(stored, true);
    const int gpm = profileIndex(QStringLiteral("GPM"));
    CHECK(gpm >= 0);
    CHECK(manager.storeProfileInProject(gpm));
    CHECK(written.has_value() && written->has_value());
    const NetworkPresentationUnits &copy = **written;
    CHECK(copy.name == manager.profiles().at(gpm).name);
    CHECK(copy.derived_from_profile == QStringLiteral("GPM"));
    CHECK(copy.units.size() == manager.profiles().at(gpm).units.size());
    CHECK(canonicallySorted(copy));
    bool has_gpm_flow = false;
    for (const NetworkPresentationUnit &choice : copy.units)
        if (choice.quantity == flow && choice.unit == QStringLiteral("US gal/min")) has_gpm_flow = true;
    CHECK(has_gpm_flow);

    written.reset();
    CHECK(manager.removeProjectUnits());
    CHECK(written.has_value() && !written->has_value());

    // Local display source: the project units stay stored but are not shown.
    manager.setDisplaySource(UnitProfileManager::DisplaySource::Local);
    CHECK(!manager.projectUnitsInEffect());
    CHECK(manager.effectiveUnits().value(flow).toString() == local_flow);
    CHECK(manager.presentation().unit(flow) == local_flow);
    GuiPreferences::reload(); // persisted, not only cached
    CHECK(GuiPreferences::value(QStringLiteral("units"), QStringLiteral("display_source")).toString()
          == QStringLiteral("local"));
    manager.setDisplaySource(UnitProfileManager::DisplaySource::Project);
    CHECK(manager.presentation().unit(flow) == QStringLiteral("US gal/min"));

    // Editing the local profile never changes the project units (copies).
    const int created = manager.profiles().size();
    CHECK(manager.create(0, QStringLiteral("Mine")));
    CHECK(manager.profiles().size() == created + 1);
    CHECK(manager.setUnit(manager.activeIndex(), flow, QStringLiteral("L/s")));
    CHECK(manager.effectiveUnits().value(flow).toString() == QStringLiteral("US gal/min"));

    // A network without project units shows the local profile again.
    manager.setNetworkProjectUnits(std::nullopt, true);
    CHECK(!manager.projectUnitsInEffect());
    CHECK(manager.effectiveUnits().value(flow).toString() == QStringLiteral("L/s"));

    // Import helper: allowed choices only, canonical order, Model name limit.
    const NetworkPresentationUnits imported = UnitProfileManager::presentationUnitsFrom(
        QString(200, QLatin1Char('x')), QStringLiteral("LPS"),
        QJsonObject{{flow, QStringLiteral("L/s")}, {pressure, QStringLiteral("bar")},
                    {QStringLiteral("no_such_quantity"), QStringLiteral("m")}});
    CHECK(imported.name.size() == 128);
    CHECK(imported.units.size() == 2);
    CHECK(imported.units.at(0).quantity == pressure);
    CHECK(canonicallySorted(imported));
    manager.setProjectUnitsWriter({});
    return 0;
}
