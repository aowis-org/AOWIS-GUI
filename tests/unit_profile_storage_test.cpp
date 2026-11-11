#include "config/unit_profile_storage.h"
#include "config/gui_config_store.h"
#include "config/gui_preferences.h"
#include "config/unit_presentation_context.h"
#include "config/unit_profile_notifications.h"
#include "import/epanet_js_unit_profile.h"
#include <aowis/model/units/conversion.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <aowis/config/object_store.h>
#include "unit_test_check.h"
#include <cmath>
#include <optional>
#include <memory>

namespace {
int storedRecords(const aowis::config::ObjectCollection &collection)
{
    const aowis::config::Result<aowis::config::ObjectListing> listing = collection.list();
    return listing ? static_cast<int>(listing.value().objects.size()) : -1;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    // Isolates the GUI's AOWIS-CONFIG store; must precede QCoreApplication.
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QCoreApplication application(argc, argv);
    // Nothing stored: no custom profiles, the canonical profile selected.
    ConfigUnitProfileStorage storage;
    const UnitProfileSnapshot empty = storage.load();
    CHECK(empty.custom_profiles.empty());
    CHECK(empty.active_id.isEmpty());
    CHECK(empty.legacy_active_name == QStringLiteral("AOWIS Canonical"));

    const aowis::config::OwnerStore *store = guiConfigStore();
    CHECK(store != nullptr);
    const aowis::config::Result<aowis::config::ObjectCollection> records = store->collection("unit-profiles");
    CHECK(records.hasValue());
    CHECK(QFileInfo(directory.filePath(QStringLiteral("aowis/gui/unit-profiles"))).isDir());

    UnitProfileSnapshot first;
    first.active_id = QStringLiteral("profile-one");
    first.legacy_active_name = QStringLiteral("My units");
    first.custom_profiles.push_back({"profile-one", "My units", {{"pressure", "psi"}, {"link_diameter", "in"}}, false});
    CHECK(storage.save(first));
    CHECK(storedRecords(records.value()) == 1);
    // The selection is a preference of the same store, not an INI key.
    GuiPreferences::reload();
    CHECK(GuiPreferences::value(QStringLiteral("units"), QStringLiteral("selected_profile_id")).toString()
          == QStringLiteral("profile-one"));
    const UnitProfileSnapshot restored = storage.load();
    CHECK(restored.active_id == first.active_id);
    CHECK(restored.legacy_active_name == first.legacy_active_name);
    CHECK(restored.custom_profiles.size() == 1);
    CHECK(restored.custom_profiles[0].id == "profile-one");
    // A pressure preference stays a fallback, not an explicit head override.
    CHECK(restored.custom_profiles[0].overrides == first.custom_profiles[0].overrides);
    CHECK(restored.custom_profiles[0].overrides.find("pressure_head")
          == restored.custom_profiles[0].overrides.end());
    CHECK(aowis::units::resolvedUnit(restored.custom_profiles[0], "pressure_head") == "psi");

    // An unchanged save writes nothing.
    const aowis::config::Result<aowis::config::ObjectListing> before = records.value().list();
    CHECK(before.hasValue() && before.value().objects.size() == 1);
    CHECK(storage.save(first));
    const aowis::config::Result<aowis::config::ObjectListing> after = records.value().list();
    CHECK(after.hasValue() && after.value().objects.size() == 1
          && after.value().objects.front().revision == before.value().objects.front().revision);

    // An explicit metre choice must override that fallback even after a
    // storage round trip (its value is canonical, but its intent is not).
    // Saving a different list replaces the old record.
    UnitProfileSnapshot explicit_head_snapshot;
    explicit_head_snapshot.active_id = QStringLiteral("explicit-pressure-head");
    aowis::units::UnitProfile explicit_head{"explicit-pressure-head", "Explicit pressure head", {}, false};
    CHECK(aowis::units::setProfileUnit(explicit_head, "pressure", "psi"));
    CHECK(aowis::units::setProfileUnit(explicit_head, "pressure_head", "m"));
    CHECK(aowis::units::resolvedUnit(explicit_head, "pressure_head") == "m");
    explicit_head_snapshot.custom_profiles.push_back(explicit_head);
    CHECK(storage.save(explicit_head_snapshot));
    CHECK(storedRecords(records.value()) == 1);
    const UnitProfileSnapshot explicit_head_restored = storage.load();
    CHECK(explicit_head_restored.custom_profiles.size() == 1);
    CHECK(explicit_head_restored.custom_profiles.front().overrides == explicit_head.overrides);
    CHECK(aowis::units::resolvedUnit(explicit_head_restored.custom_profiles.front(),
                                      "pressure_head") == "m");
    // Profile names are case-sensitive in both the Model and persisted settings:
    // exact duplicates are rejected, but differently cased names are distinct.
    UnitProfileSnapshot case_snapshot;
    case_snapshot.active_id = QStringLiteral("case-upper");
    case_snapshot.custom_profiles.push_back({"case-upper", "Field", {}, false});
    case_snapshot.custom_profiles.push_back({"case-lower", "field", {}, false});
    case_snapshot.custom_profiles.push_back({"case-duplicate", "Field", {}, false});
    case_snapshot.custom_profiles.push_back({"builtin-different-case", "cmh", {}, false});
    case_snapshot.custom_profiles.push_back({"builtin-exact", "CMH", {}, false});
    CHECK(storage.save(case_snapshot));
    const UnitProfileSnapshot case_restored = storage.load();
    CHECK(case_restored.custom_profiles.size() == 3);
    CHECK(case_restored.custom_profiles.at(0).id == "case-upper");
    CHECK(case_restored.custom_profiles.at(1).id == "case-lower");
    CHECK(case_restored.custom_profiles.at(2).id == "builtin-different-case");

    // Records of a newer format, unknown unit choices and unreadable records:
    // never interpreted as profiles, never deleted by a save.
    const aowis::config::Result<aowis::config::StoredObject> newer = records.value().create(
        "{\"unit_profile_version\":2,\"profile_id\":\"newer\",\"name\":\"Newer\"}");
    CHECK(newer.hasValue());
    const aowis::config::Result<aowis::config::StoredObject> partial = records.value().create(
        "{\"unit_profile_version\":1,\"profile_id\":\"partial\",\"name\":\"Partial\",\"position\":9,"
        "\"overrides\":{\"pressure\":\"bar\",\"pressure_bogus\":\"x\",\"link_diameter\":\"furlong\"}}");
    CHECK(partial.hasValue());
    const UnitProfileSnapshot mixed = storage.load();
    CHECK(mixed.custom_profiles.size() == 4);
    CHECK(mixed.custom_profiles.back().id == "partial");
    CHECK(mixed.custom_profiles.back().overrides.size() == 1);
    CHECK(mixed.custom_profiles.back().overrides.at("pressure") == "bar");
    UnitProfileSnapshot without_partial = mixed;
    without_partial.custom_profiles.pop_back();
    CHECK(storage.save(without_partial));
    CHECK(records.value().read(newer.value().info.id).hasValue());
    CHECK(!records.value().read(partial.value().info.id).hasValue());
    CHECK(storage.load().custom_profiles.size() == 3);

    // Independent semantic choices must survive persistence even when the
    // explicit choice equals canonical and the inherited flow is noncanonical.
    UnitProfileSnapshot semantic_snapshot;
    semantic_snapshot.active_id = QStringLiteral("semantic-profile");
    aowis::units::UnitProfile semantic{"semantic-profile", "Semantic", {}, false};
    CHECK(aowis::units::setProfileUnit(semantic, "volumetric_flow_rate", "L/s"));
    CHECK(aowis::units::setProfileUnit(semantic, "base_demand_flow_rate", "m3/h"));
    CHECK(aowis::units::setProfileUnit(semantic, "customer_demand_flow_rate", "US gal/min"));
    CHECK(aowis::units::setProfileUnit(semantic, "water_level", "ft"));
    CHECK(aowis::units::setProfileUnit(semantic, "tank_minimum_level", "m"));
    CHECK(aowis::units::setProfileUnit(semantic, "volume", "ft3"));
    CHECK(aowis::units::setProfileUnit(semantic, "tank_minimum_volume", "m3"));
    semantic_snapshot.custom_profiles.push_back(semantic);
    CHECK(storage.save(semantic_snapshot));
    const UnitProfileSnapshot semantic_reloaded = storage.load();
    CHECK(semantic_reloaded.custom_profiles.size() == 1);
    CHECK(semantic_reloaded.custom_profiles.front().overrides == semantic.overrides);
    CHECK(aowis::units::resolvedUnit(semantic_reloaded.custom_profiles.front(),
                                      "base_demand_flow_rate") == "m3/h");
    CHECK(aowis::units::resolvedUnit(semantic_reloaded.custom_profiles.front(),
                                      "customer_demand_flow_rate") == "US gal/min");
    CHECK(aowis::units::resolvedUnit(semantic_reloaded.custom_profiles.front(),
                                      "tank_initial_level") == "ft");
    CHECK(aowis::units::resolvedUnit(semantic_reloaded.custom_profiles.front(),
                                      "tank_minimum_level") == "m");
    // Minimum-volume overrides survive storage, but the effective unit
    // follows the general volume preference.
    CHECK(semantic_reloaded.custom_profiles.front().overrides.at("tank_minimum_volume") == "m3");
    CHECK(aowis::units::resolvedUnit(semantic_reloaded.custom_profiles.front(),
                                     "tank_minimum_volume") == "ft3");
    // Profiles without the newer inherited keys inherit dynamically.
    UnitProfileSnapshot inheriting_snapshot;
    aowis::units::UnitProfile inheriting{"inheriting", "Inheriting", {}, false};
    CHECK(aowis::units::setProfileUnit(inheriting, "volumetric_flow_rate", "L/s"));
    CHECK(aowis::units::setProfileUnit(inheriting, "water_level", "ft"));
    inheriting_snapshot.custom_profiles.push_back(inheriting);
    CHECK(storage.save(inheriting_snapshot));
    const UnitProfileSnapshot inheriting_reloaded = storage.load();
    CHECK(inheriting_reloaded.custom_profiles.size() == 1);
    CHECK(aowis::units::resolvedUnit(inheriting_reloaded.custom_profiles.front(),
                                      "base_demand_flow_rate") == "L/s");
    CHECK(aowis::units::resolvedUnit(inheriting_reloaded.custom_profiles.front(),
                                      "tank_initial_level") == "ft");
    // Integration: source-unit preferences -> custom profile -> disk -> active
    // presentation -> repeated profile switches. None of these operations may
    // modify a measurement imported into the canonical hydraulic model.
    const QJsonObject imported_source_units{
        {QStringLiteral("flow"), QStringLiteral("gal/min")},
        {QStringLiteral("baseDemand"), QStringLiteral("m^3/h")},
        {QStringLiteral("customerDemand"), QStringLiteral("l/s")},
        {QStringLiteral("level"), QStringLiteral("ft")},
        {QStringLiteral("initialLevel"), QStringLiteral("m")},
        {QStringLiteral("minLevel"), QStringLiteral("ft")},
        {QStringLiteral("maxLevel"), QStringLiteral("m")},
        {QStringLiteral("volume"), QStringLiteral("ft^3")},
        {QStringLiteral("minVolume"), QStringLiteral("m^3")},
        {QStringLiteral("roughness"), QStringLiteral("millift")},
        {QStringLiteral("pressure"), QStringLiteral("kPa")},
        {QStringLiteral("waterAge"), QStringLiteral("seconds")}
    };
    QStringList imported_unmapped;
    const QJsonObject imported_selections = EpanetJsUnitProfile::selections(
        imported_source_units, &imported_unmapped);
    CHECK(imported_unmapped.isEmpty());
    CHECK(imported_selections.value(QStringLiteral("volumetric_flow_rate")).toString()
           == QStringLiteral("US gal/min"));
    CHECK(imported_selections.value(QStringLiteral("base_demand_flow_rate")).toString()
           == QStringLiteral("m3/h"));
    CHECK(imported_selections.value(QStringLiteral("customer_demand_flow_rate")).toString()
           == QStringLiteral("L/s"));
    CHECK(imported_selections.value(QStringLiteral("tank_initial_level")).toString()
           == QStringLiteral("m"));
    CHECK(imported_selections.value(QStringLiteral("tank_minimum_level")).toString()
           == QStringLiteral("ft"));
    CHECK(imported_selections.value(QStringLiteral("tank_maximum_level")).toString()
           == QStringLiteral("m"));
    CHECK(imported_selections.value(QStringLiteral("tank_minimum_volume")).toString()
           == QStringLiteral("m3"));
    CHECK(imported_selections.value(QStringLiteral("darcy_weisbach_roughness_height")).toString()
           == QStringLiteral("millift"));
    CHECK(imported_selections.value(QStringLiteral("water_age")).toString()
           == QStringLiteral("s"));

    aowis::units::UnitProfile imported_profile;
    imported_profile.id = "imported-integration";
    imported_profile.name = "Imported integration";
    for (QJsonObject::const_iterator selection = imported_selections.constBegin();
         selection != imported_selections.constEnd(); ++selection) {
        CHECK(aowis::units::setProfileUnit(imported_profile,
                                            selection.key().toStdString(),
                                            selection.value().toString().toStdString()));
    }
    // Explicit canonical values must survive a noncanonical parent selection.
    CHECK(imported_profile.overrides.at("base_demand_flow_rate") == "m3/h");
    CHECK(imported_profile.overrides.at("tank_initial_level") == "m");
    CHECK(imported_profile.overrides.at("tank_minimum_volume") == "m3");
    ConfigUnitProfileStorage &import_storage = storage;
    UnitProfileSnapshot imported_snapshot;
    imported_snapshot.active_id = QString::fromStdString(imported_profile.id);
    imported_snapshot.legacy_active_name = QString::fromStdString(imported_profile.name);
    imported_snapshot.custom_profiles.push_back(imported_profile);
    CHECK(import_storage.save(imported_snapshot));
    const UnitProfileSnapshot imported_restored = import_storage.load();
    CHECK(imported_restored.active_id == imported_snapshot.active_id);
    CHECK(imported_restored.custom_profiles.size() == 1);
    CHECK(imported_restored.custom_profiles.front().overrides == imported_profile.overrides);

    aowis::units::UnitProfileCollection integration_collection;
    CHECK(integration_collection.add(imported_restored.custom_profiles.front()));
    CHECK(integration_collection.select(imported_profile.id));
    const aowis::units::UnitProfile *active_profile = integration_collection.find(
        integration_collection.activeId());
    CHECK(active_profile != nullptr);
    QJsonObject active_units;
    QJsonObject canonical_units;
    const aowis::units::UnitProfile canonical_profile = aowis::units::canonicalProfile();
    for (const aowis::units::UnitField &field : aowis::units::unit_fields) {
        const QString key = QString::fromStdString(std::string(field.key));
        active_units.insert(key, QString::fromStdString(
            std::string(aowis::units::resolvedUnit(*active_profile, field.key))));
        canonical_units.insert(key, QString::fromStdString(
            std::string(aowis::units::resolvedUnit(canonical_profile, field.key))));
    }
    UnitPresentationContext imported_presentation;
    imported_presentation.initialize(active_units);
    CHECK(imported_presentation.unit(QStringLiteral("base_demand_flow_rate"))
           == QStringLiteral("m3/h"));
    CHECK(imported_presentation.unit(QStringLiteral("customer_demand_flow_rate"))
           == QStringLiteral("L/s"));

    const double canonical_flow_m3h = 12.345678901234;
    const double canonical_roughness_mm = 0.09123456789;
    const double canonical_tank_volume_m3 = 123.456789;
    const UnitPresentationContext::Generation initial_import_generation =
        imported_presentation.generation();
    for (int iteration = 0; iteration < 8; ++iteration) {
        CHECK(imported_presentation.update(canonical_units));
        CHECK(imported_presentation.unit(QStringLiteral("customer_demand_flow_rate"))
               == QStringLiteral("m3/h"));
        CHECK(imported_presentation.update(active_units));
        const std::optional<double> display_flow = aowis::units::convert(
            "customer_demand_flow_rate", canonical_flow_m3h, "m3/h",
            imported_presentation.unit(QStringLiteral("customer_demand_flow_rate")).toStdString());
        CHECK(display_flow.has_value());
        const std::optional<double> restored_flow = aowis::units::convert(
            "customer_demand_flow_rate", *display_flow, "L/s", "m3/h");
        CHECK(restored_flow.has_value());
        CHECK(std::fabs(*restored_flow - canonical_flow_m3h) < 1e-11);
        const std::optional<double> display_roughness = aowis::units::convert(
            "darcy_weisbach_roughness_height", canonical_roughness_mm, "mm", "millift");
        CHECK(display_roughness.has_value());
        const std::optional<double> restored_roughness = aowis::units::convert(
            "darcy_weisbach_roughness_height", *display_roughness, "millift", "mm");
        CHECK(restored_roughness.has_value());
        CHECK(std::fabs(*restored_roughness - canonical_roughness_mm) < 1e-12);
        const std::optional<double> display_volume = aowis::units::convert(
            "volume", canonical_tank_volume_m3, "m3",
            imported_presentation.unit(QStringLiteral("volume")).toStdString());
        CHECK(display_volume.has_value());
        const std::optional<double> restored_volume = aowis::units::convert(
            "volume", *display_volume, "ft3", "m3");
        CHECK(restored_volume.has_value());
        CHECK(std::fabs(*restored_volume - canonical_tank_volume_m3) < 1e-10);
        const std::optional<double> minimum_volume_display = aowis::units::convert(
            "tank_minimum_volume", canonical_tank_volume_m3, "m3",
            imported_presentation.unit(QStringLiteral("tank_minimum_volume")).toStdString());
        CHECK(minimum_volume_display.has_value());
        CHECK(imported_presentation.unit(QStringLiteral("tank_minimum_volume"))
              == imported_presentation.unit(QStringLiteral("volume")));
        CHECK(std::fabs(*minimum_volume_display - *display_volume) < 1e-10);
        CHECK(canonical_flow_m3h == 12.345678901234);
        CHECK(canonical_roughness_mm == 0.09123456789);
        CHECK(canonical_tank_volume_m3 == 123.456789);
    }
    CHECK(imported_presentation.generation() == initial_import_generation + 16);
    CHECK(!imported_presentation.update(active_units));
    CHECK(imported_presentation.generation() == initial_import_generation + 16);
    // Opting out of activation must leave the active profile unchanged.
    CHECK(integration_collection.select("canonical"));
    const std::string previous_active = integration_collection.activeId();
    aowis::units::UnitProfile inactive_import = imported_profile;
    inactive_import.id = "imported-not-activated";
    inactive_import.name = "Imported without activation";
    CHECK(integration_collection.add(inactive_import));
    CHECK(previous_active == "canonical");
    CHECK(integration_collection.activeId() == previous_active);
    // Active-unit generation changes only when the effective mapping changes.
    UnitPresentationContext presentation;
    presentation.initialize(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("kPa")}});
    QObject subscriber;
    int callbacks = 0;
    UnitPresentationContext::Generation observed_generation = 0;
    presentation.subscribe(&subscriber, [&](UnitPresentationContext::Generation generation) {
        ++callbacks;
        observed_generation = generation;
    });
    const UnitPresentationContext::Generation original_generation = presentation.generation();
    CHECK(!presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("kPa")}}));
    CHECK(presentation.generation() == original_generation);
    CHECK(callbacks == 0);
    CHECK(presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("psi")}}));
    CHECK(presentation.generation() == original_generation + 1);
    CHECK(observed_generation == presentation.generation());
    CHECK(callbacks == 1);
    CHECK(presentation.unit(QStringLiteral("pressure")) == QStringLiteral("psi"));
    // A destroyed QObject cannot receive callbacks.
    {
        QObject temporary;
        presentation.subscribe(&temporary, [&](UnitPresentationContext::Generation) { ++callbacks; });
    }
    CHECK(presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("bar")}}));
    CHECK(callbacks == 2);
    CHECK(presentation.subscriberCount() == 1);

    // Owner destruction must remove the subscription and release captured
    // resources immediately, without requiring another profile switch.
    const qsizetype persistent_listeners = presentation.subscriberCount();
    for (int i = 0; i < 500; ++i) {
        std::weak_ptr<int> captured;
        {
            QObject temporary;
            const std::shared_ptr<int> payload = std::make_shared<int>(i);
            captured = payload;
            presentation.subscribe(&temporary, [payload](UnitPresentationContext::Generation) {});
            CHECK(presentation.subscriberCount() == persistent_listeners + 1);
            CHECK(!captured.expired());
        }
        CHECK(presentation.subscriberCount() == persistent_listeners);
        CHECK(captured.expired());
    }
    CHECK(callbacks == 2);

    // A notification can destroy another subscribed owner; its callback must
    // not run from the notification snapshot, and iteration stays valid.
    {
        QObject first;
        std::unique_ptr<QObject> second = std::make_unique<QObject>();
        int first_calls = 0;
        int destroyed_calls = 0;
        presentation.subscribe(&first, [&](UnitPresentationContext::Generation) {
            ++first_calls;
            second.reset();
        });
        presentation.subscribe(second.get(), [&](UnitPresentationContext::Generation) {
            ++destroyed_calls;
        });
        CHECK(presentation.subscriberCount() == persistent_listeners + 2);
        CHECK(presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("psi")}}));
        CHECK(first_calls == 1);
        CHECK(destroyed_calls == 0);
        CHECK(presentation.subscriberCount() == persistent_listeners + 1);
    }
    CHECK(presentation.subscriberCount() == persistent_listeners);

    // A listener registered inside a notification joins only the next
    // notification, never the snapshot already being traversed.
    {
        QObject registering_owner;
        QObject new_owner;
        bool registered = false;
        int new_owner_calls = 0;
        presentation.subscribe(&registering_owner, [&](UnitPresentationContext::Generation) {
            if (!registered) {
                registered = true;
                presentation.subscribe(&new_owner,
                    [&](UnitPresentationContext::Generation) { ++new_owner_calls; });
            }
        });
        CHECK(presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("bar")}}));
        CHECK(new_owner_calls == 0);
        CHECK(presentation.update(QJsonObject{{QStringLiteral("pressure"), QStringLiteral("psi")}}));
        CHECK(new_owner_calls == 1);
    }
    CHECK(presentation.subscriberCount() == persistent_listeners);

    // A short-lived context must not leave connections to an owner that
    // outlives it; QObject receiver destruction disconnects them.
    QObject long_lived_owner;
    {
        UnitPresentationContext short_lived_context;
        short_lived_context.subscribe(&long_lived_owner,
            [](UnitPresentationContext::Generation) {});
        CHECK(short_lived_context.subscriberCount() == 1);
    }

    // Metadata notifications need the same lifecycle guarantees as numerical
    // presentation notifications, including source suppression.
    UnitProfileNotifications &notifications = UnitProfileNotifications::instance();
    const qsizetype metadata_baseline = notifications.subscriberCount();
    {
        QObject source;
        int calls = 0;
        notifications.subscribe(&source, [&] { ++calls; });
        CHECK(notifications.subscriberCount() == metadata_baseline + 1);
        notifications.publish(&source);
        CHECK(calls == 0);
        notifications.publish(nullptr);
        CHECK(calls == 1);
    }
    CHECK(notifications.subscriberCount() == metadata_baseline);
    for (int i = 0; i < 500; ++i) {
        std::weak_ptr<int> captured;
        {
            QObject temporary;
            const std::shared_ptr<int> payload = std::make_shared<int>(i);
            captured = payload;
            notifications.subscribe(&temporary, [payload] {});
            CHECK(notifications.subscriberCount() == metadata_baseline + 1);
        }
        CHECK(notifications.subscriberCount() == metadata_baseline);
        CHECK(captured.expired());
    }
    {
        QObject first;
        std::unique_ptr<QObject> second = std::make_unique<QObject>();
        int destroyed_calls = 0;
        notifications.subscribe(&first, [&] { second.reset(); });
        notifications.subscribe(second.get(), [&] { ++destroyed_calls; });
        notifications.publish(nullptr);
        CHECK(destroyed_calls == 0);
        CHECK(notifications.subscriberCount() == metadata_baseline + 1);
    }
    CHECK(notifications.subscriberCount() == metadata_baseline);
    return 0;
}
