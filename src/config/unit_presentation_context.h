#pragma once

#include "config/unit_quantity_key.h"

#include "numeric_text.h"
#include <aowis/model/units/unit_profile.h>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <cmath>
#include <utility>

// Effective GUI display units: the local profile, or the open network's project
// units (see UnitProfileManager). This context never converts or writes model
// measurements.
class UnitPresentationContext : public QObject
{
public:
    using Generation = quint64;

    [[nodiscard]] Generation generation() const { return this->generation_; }
    [[nodiscard]] double specificGravity() const { return this->specific_gravity_; }
    bool setSpecificGravity(double value) {
        if (!std::isfinite(value) || value <= 0.0 || value == this->specific_gravity_)
            return false;
        this->specific_gravity_ = value;
        notifyPresentationChanged();
        return true;
    }
    // Invalid/missing project data must not leave the previous network's
    // specific gravity active in this shared presentation context.
    bool setNetworkSpecificGravity(double value) {
        return setSpecificGravity(std::isfinite(value) && value > 0.0 ? value : 1.0);
    }
    [[nodiscard]] qsizetype subscriberCount() const { return this->listeners_.size(); }
    [[nodiscard]] QString unit(const QString &quantity) const {
        // The legacy minimum-volume key never has an independent display unit,
        // even when older profile JSON still contains its own selection.
        if (quantity == AOWIS_QSTRING_QUANTITY("tank_minimum_volume"))
            return unit(AOWIS_QSTRING_QUANTITY("volume"));
        const QString selected = this->effective_units_.value(quantity).toString();
        if (!selected.isEmpty()) return selected;
        // Partial/legacy profiles inherit new semantic display quantities.
        const std::string_view parent = aowis::units::inheritedUnitQuantity(
            quantity.toStdString());
        if (parent.empty()) return {};
        return this->effective_units_.value(
            QString::fromLatin1(parent.data(), static_cast<qsizetype>(parent.size()))).toString();
    }
    [[nodiscard]] const QJsonObject &effectiveUnits() const { return this->effective_units_; }

    [[nodiscard]] NumericFormatPolicy formatPolicy(const QString &quantity = {}) const {
        return this->format_overrides_.value(quantity, this->default_format_);
    }
    [[nodiscard]] bool hasFormatPolicyOverride(const QString &quantity) const {
        return this->format_overrides_.contains(quantity);
    }
    [[nodiscard]] QLocale locale() const { return this->locale_; }
    bool setLocale(const QLocale &locale) {
        if (locale == this->locale_) return false;
        this->locale_ = locale;
        notifyPresentationChanged();
        return true;
    }
    bool setFormatPolicy(const NumericFormatPolicy &policy, const QString &quantity = {}) {
        if (!policy.valid()) return false;
        if (quantity.isEmpty()) {
            if (policy == this->default_format_) return false;
            this->default_format_ = policy;
        } else {
            const bool changed = !(formatPolicy(quantity) == policy);
            this->format_overrides_.insert(quantity, policy);
            if (!changed) return false;
        }
        notifyPresentationChanged();
        return true;
    }
    bool clearFormatPolicy(const QString &quantity) {
        const NumericFormatPolicy previous = formatPolicy(quantity);
        if (!this->format_overrides_.remove(quantity)) return false;
        if (previous == formatPolicy(quantity)) return false;
        notifyPresentationChanged();
        return true;
    }

    // The manager initializes this once, after loading the selected profile.
    void initialize(const QJsonObject &units) {
        this->effective_units_ = units;
    }

    // Returns false if only the profile identity/name changed, not its effective units.
    bool update(const QJsonObject &units) {
        if (this->effective_units_ == units) return false;
        this->effective_units_ = units;
        notifyPresentationChanged();
        return true;
    }

    // Subscribers should mark themselves stale. An owner's destruction removes
    // its listener immediately, even if there are no further profile changes.
    // QObject's receiver context disconnects the cleanup when this context dies.
    void subscribe(QObject *owner, std::function<void(Generation)> refresh) {
        if (!owner || owner == this || !refresh) return;
        const quint64 id = ++this->next_subscription_id_;
        this->listeners_.insert(id, Listener{owner, std::move(refresh)});
        QObject::connect(owner, &QObject::destroyed, this, [this, id] {
            this->listeners_.remove(id);
        });
    }

private:
    void notifyPresentationChanged() {
        const Generation target = ++this->generation_;
        // QMap copies are implicitly shared. Add/remove/destroy during a callback
        // cannot invalidate iteration or call a subsequently destroyed owner.
        const QMap<quint64, Listener> snapshot = this->listeners_;
        for (QMap<quint64, Listener>::const_iterator it = snapshot.cbegin();
             it != snapshot.cend(); ++it) {
            const Listener &listener = it.value();
            if (listener.owner) listener.refresh(target);
        }
    }
    struct Listener {
        QPointer<QObject> owner;
        std::function<void(Generation)> refresh;
    };
    NumericFormatPolicy default_format_;
    QHash<QString, NumericFormatPolicy> format_overrides_;
    QLocale locale_;
    Generation generation_ = 1;
    double specific_gravity_ = 1.0;
    QJsonObject effective_units_;
    quint64 next_subscription_id_ = 0;
    QMap<quint64, Listener> listeners_;
};
