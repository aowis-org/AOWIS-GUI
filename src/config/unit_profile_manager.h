#pragma once

#include "config/unit_profile_storage.h"
#include "config/gui_configuration.h"
#include "config/unit_profile_notifications.h"
#include "config/unit_presentation_context.h"
#include <aowis/model/units/unit_profile.h>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QUuid>

// Qt adapter: a single Model-owned collection shared by Settings and the toolbar.
// Custom profiles persist in AOWIS-CONFIG, the selection in the GUI INI
// (config/unit_profile_storage.h).
class UnitProfileManager {
public:
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
    UnitProfileManager() : storage_(guiConfigurationFilePath()) {
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
        this->presentation_.initialize(this->views_.at(this->activeIndex()).units);
    }
    void notify(QObject *origin) {
        // Refresh active unit mappings before legacy profile-list notifications.
        this->presentation_.update(this->views_.at(this->activeIndex()).units);
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
};
