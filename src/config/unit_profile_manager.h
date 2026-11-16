#pragma once

#include "config/unit_profile_storage.h"
#include "config/gui_configuration.h"
#include "config/gui_preferences.h"
#include "config/unit_profile_notifications.h"
#include "config/unit_presentation_context.h"
#include <aowis/model/units/unit_profile.h>
#include <aowis/model/hydraulic/network_presentation_units.h>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>
#include <QUuid>
#include <algorithm>
#include <functional>
#include <optional>

// Qt adapter: a single Model-owned collection shared by Settings and the toolbar.
// Custom profiles and the selection persist in AOWIS-CONFIG
// (config/unit_profile_storage.h).
//
// Project units: the open network may store its own presentation units
// (NetworkHydraulic::presentation_units, committed with the network). The
// display source preference (group "units", key "display_source":
// "project" = default, "local") decides which apply. Effective units are the
// project units when the source is "project" and the network stores units;
// quantities the project does not list fall back to the active local profile.
// Project units are copies: editing a local profile never changes them.
class UnitProfileManager {
public:
    enum class DisplaySource { Project, Local };
    struct ProjectUnits {
        bool present = false;    // the open network stores units
        bool editable = false;   // a private draft accepts a change now
        QString name;
        QString derived_from_profile;
        QJsonObject units;       // as stored (may be partial)
    };
    // Registered by the main window: writes a root-metadata edit into the
    // private draft (empty optional = remove). Returns false if not possible.
    using ProjectUnitsWriter = std::function<bool(const std::optional<NetworkPresentationUnits> &)>;

    struct Profile {
        QString id;
        QString name;
        QJsonObject units;
        bool builtin = false;
    };
    static UnitProfileManager &instance() {
        static UnitProfileManager manager;
        return manager;
    }
    [[nodiscard]] const QVector<Profile> &profiles() const { return this->views_; }
    [[nodiscard]] DisplaySource displaySource() const { return this->display_source_; }
    [[nodiscard]] const ProjectUnits &projectUnits() const { return this->project_; }
    // True when the views show the network's stored units.
    [[nodiscard]] bool projectUnitsInEffect() const {
        return this->display_source_ == DisplaySource::Project && this->project_.present;
    }
    // Display units actually used by every unit-aware view.
    [[nodiscard]] QJsonObject effectiveUnits() const {
        QJsonObject result = this->views_.at(this->activeIndex()).units;
        if (!this->projectUnitsInEffect()) return result;
        for (QJsonObject::const_iterator it = this->project_.units.constBegin();
             it != this->project_.units.constEnd(); ++it)
            if (result.contains(it.key())) result.insert(it.key(), it.value());
        return result;
    }
    void setDisplaySource(DisplaySource source, QObject *origin = nullptr) {
        if (this->display_source_ == source) return;
        this->display_source_ = source;
        (void)GuiPreferences::update(QStringLiteral("units"),
            QJsonObject{{QStringLiteral("display_source"),
                         source == DisplaySource::Local ? QStringLiteral("local") : QStringLiteral("project")}});
        this->notify(origin);
    }
    // Called whenever the open network or its editability changes.
    void setNetworkProjectUnits(const std::optional<NetworkPresentationUnits> &stored, bool editable,
                                QObject *origin = nullptr) {
        ProjectUnits next;
        next.editable = editable;
        if (stored) {
            next.present = true;
            next.name = stored->name;
            next.derived_from_profile = stored->derived_from_profile;
            for (const NetworkPresentationUnit &choice : stored->units)
                if (aowis::units::isAllowedUnit(choice.quantity.toStdString(), choice.unit.toStdString()))
                    next.units.insert(choice.quantity, choice.unit);
        }
        if (next.present == this->project_.present && next.editable == this->project_.editable
            && next.name == this->project_.name && next.units == this->project_.units
            && next.derived_from_profile == this->project_.derived_from_profile) return;
        this->project_ = std::move(next);
        this->notify(origin);
    }
    void setProjectUnitsWriter(ProjectUnitsWriter writer) { this->project_writer_ = std::move(writer); }
    // Canonical project units (allowed choices only, sorted by quantity, as
    // the Model requires) from display selections.
    [[nodiscard]] static NetworkPresentationUnits presentationUnitsFrom(
            const QString &name, const QString &derived_from_profile, const QJsonObject &selections) {
        NetworkPresentationUnits units;
        units.name = name.trimmed().left(128); // Model limit
        units.derived_from_profile = derived_from_profile.left(128);
        for (QJsonObject::const_iterator it = selections.constBegin(); it != selections.constEnd(); ++it) {
            const QString unit = it.value().toString();
            if (aowis::units::isAllowedUnit(it.key().toStdString(), unit.toStdString()))
                units.units.append(NetworkPresentationUnit{it.key(), unit});
        }
        std::sort(units.units.begin(), units.units.end(),
                  [](const NetworkPresentationUnit &left, const NetworkPresentationUnit &right) {
                      return QString::compare(left.quantity, right.quantity, Qt::CaseSensitive) < 0;
                  });
        return units;
    }
    // Stores a complete copy of a local profile as the network's project units.
    bool storeProfileInProject(int index) {
        if (!this->valid(index) || !this->project_.editable || !this->project_writer_) return false;
        const Profile &profile = this->views_.at(index);
        return this->project_writer_(presentationUnitsFrom(profile.name, profile.id, profile.units));
    }
    bool removeProjectUnits() {
        if (!this->project_.present || !this->project_.editable || !this->project_writer_) return false;
        return this->project_writer_(std::nullopt);
    }
    [[nodiscard]] UnitPresentationContext &presentation() { return this->presentation_; }
    [[nodiscard]] const UnitPresentationContext &presentation() const { return this->presentation_; }
    [[nodiscard]] int activeIndex() const {
        const QString id = QString::fromStdString(this->collection_.activeId());
        for (int i = 0; i < this->views_.size(); ++i)
            if (this->views_.at(i).id == id) return i;
        return 0;
    }
    bool select(int index, QObject *source = nullptr) {
        if (!this->valid(index)) return false;
        if (!this->collection_.select(this->views_.at(index).id.toStdString())) return false;
        this->persist();
        this->notify(source);
        return true;
    }
    bool create(int source, const QString &name, QObject *origin = nullptr) {
        if (!this->valid(source) || !this->validName(name)) return false;
        aowis::units::UnitProfile profile = *this->collection_.find(this->views_.at(source).id.toStdString());
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        profile.name = name.trimmed().toStdString();
        profile.builtin = false;
        if (!this->collection_.add(std::move(profile))) return false;
        this->rebuild();
        this->select(this->views_.size()-1, origin);
        return true;
    }
    // Creates and activates an imported profile atomically, notifying the UI once.
    bool createFromUnits(const QString &name, const QJsonObject &units, bool activate = true, QObject *origin = nullptr) {
        if (!this->validName(name)) return false;
        aowis::units::UnitProfile profile;
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        profile.name = name.trimmed().toStdString();
        for (QJsonObject::const_iterator it = units.constBegin(); it != units.constEnd(); ++it) {
            if (!it.value().isString()) return false;
            const std::string key = it.key().toStdString();
            const std::string unit = it.value().toString().toStdString();
            if (!aowis::units::setProfileUnit(profile, key, unit)) return false;
        }
        const std::string id = profile.id;
        if (!this->collection_.add(std::move(profile))) return false;
        if (activate) (void)this->collection_.select(id);
        this->rebuild();
        this->persist();
        this->notify(origin);
        return true;
    }
    bool rename(int index, const QString &name, QObject *origin = nullptr) {
        if (!this->valid(index) || this->views_.at(index).builtin || !this->validName(name, index)) return false;
        if (!this->collection_.rename(this->views_.at(index).id.toStdString(), name.trimmed().toStdString())) return false;
        this->rebuild(); this->persist();
        this->notify(origin);
        return true;
    }
    bool remove(int index, QObject *origin = nullptr) {
        if (!this->valid(index) || this->views_.at(index).builtin) return false;
        if (!this->collection_.remove(this->views_.at(index).id.toStdString())) return false;
        this->rebuild(); this->persist();
        this->notify(origin);
        return true;
    }
    bool setUnit(int index, const QString &quantity, const QString &unit, QObject *origin = nullptr) {
        if (!this->valid(index)) return false;
        if (!this->collection_.setUnit(this->views_.at(index).id.toStdString(), quantity.toStdString(), unit.toStdString())) return false;
        this->rebuild(); this->persist();
        this->notify(origin);
        return true;
    }
private:
    UnitProfileManager() {
        const UnitProfileSnapshot stored = this->storage_.load();
        for (const aowis::units::UnitProfile &profile : stored.custom_profiles)
            (void)this->collection_.add(profile);
        this->rebuild();
        for (const Profile &profile : this->views_) {
            if ((!stored.active_id.isEmpty() && profile.id == stored.active_id) ||
                (stored.active_id.isEmpty() && profile.name == stored.legacy_active_name)) {
                (void)this->collection_.select(profile.id.toStdString());
                break;
            }
        }
        this->display_source_ = GuiPreferences::value(QStringLiteral("units"), QStringLiteral("display_source"))
                .toString() == QStringLiteral("local") ? DisplaySource::Local : DisplaySource::Project;
        this->presentation_.initialize(this->effectiveUnits());
    }
    void notify(QObject *origin) {
        // Refresh active unit mappings before legacy profile-list notifications.
        this->presentation_.update(this->effectiveUnits());
        UnitProfileNotifications::instance().publish(origin);
    }
    [[nodiscard]] bool valid(int index) const { return index >= 0 && index < this->views_.size(); }
    [[nodiscard]] bool validName(const QString &name, int except = -1) const {
        const QString trimmed_name = name.trimmed();
        if (trimmed_name.isEmpty()) return false;
        for (int i = 0; i < this->views_.size(); ++i)
            if (i != except && this->views_.at(i).name == trimmed_name) return false;
        return true;
    }
    void rebuild() {
        this->views_.clear();
        for (const aowis::units::UnitProfile &profile : this->collection_.profiles()) {
            Profile view;
            view.id = QString::fromStdString(profile.id);
            view.name = QString::fromStdString(profile.name);
            view.builtin = profile.builtin;
            for (const aowis::units::UnitField &field : aowis::units::unit_fields) {
                const std::string_view unit = aowis::units::resolvedUnit(profile, field.key);
                view.units.insert(QString::fromLatin1(field.key.data(), field.key.size()),
                                  QString::fromLatin1(unit.data(), unit.size()));
            }
            this->views_.append(std::move(view));
        }
    }
    void persist() const {
        UnitProfileSnapshot snapshot;
        for (const aowis::units::UnitProfile &profile : this->collection_.profiles())
            if (!profile.builtin) snapshot.custom_profiles.push_back(profile);
        snapshot.active_id = QString::fromStdString(this->collection_.activeId());
        snapshot.legacy_active_name = this->views_.at(this->activeIndex()).name;
        (void)this->storage_.save(snapshot);
    }
    ConfigUnitProfileStorage storage_;
    aowis::units::UnitProfileCollection collection_;
    QVector<Profile> views_;
    UnitPresentationContext presentation_;
    DisplaySource display_source_ = DisplaySource::Project;
    ProjectUnits project_;
    ProjectUnitsWriter project_writer_;
};
