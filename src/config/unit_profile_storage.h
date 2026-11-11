#pragma once

#include <aowis/model/units/unit_profile.h>
#include <QString>
#include <vector>
#include <utility>

// Persistence boundary. No QSettings, JSON, SQLite or HTTP in the Model.
struct UnitProfileSnapshot {
    std::vector<aowis::units::UnitProfile> custom_profiles;
    QString active_id;
    QString legacy_active_name;
};

class UnitProfileStorage {
public:
    virtual ~UnitProfileStorage() = default;
    [[nodiscard]] virtual UnitProfileSnapshot load() const = 0;
    virtual bool save(const UnitProfileSnapshot &snapshot) const = 0;
};

// Custom profiles are records of the GUI's AOWIS-CONFIG "unit-profiles"
// collection (config/gui_config_store.h), one per profile:
//   {"unit_profile_version":1, "profile_id", "name", "position",
//    "overrides":{<unit field>:<unit>}}
// overrides are stored exactly as the Model holds them (explicit canonical
// choices included), so no marker fields are needed. Records of another
// unit_profile_version, unreadable records and unknown fields are never
// interpreted, modified or deleted by a save.
//
// The selection (active_id, legacy_active_name) is a preference: group
// "units", keys selected_profile_id and selected_profile
// (config/gui_preferences.h).
class ConfigUnitProfileStorage final : public UnitProfileStorage {
public:
    [[nodiscard]] UnitProfileSnapshot load() const override;
    bool save(const UnitProfileSnapshot &snapshot) const override;
};
