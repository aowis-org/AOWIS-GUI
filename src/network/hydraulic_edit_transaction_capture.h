#ifndef HYDRAULIC_EDIT_TRANSACTION_CAPTURE_H
#define HYDRAULIC_EDIT_TRANSACTION_CAPTURE_H

#include <optional>

#include <QHash>
#include <QSet>
#include <QString>
#include <QUuid>

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/hydraulic/network_hydraulic.h>

// Captures only explicitly affected entity UUIDs, not an entire network.
// One instance corresponds to one logical user action (one future undo step).
// It produces MODEL's reversible binary event; it does not save or commit it.
class HydraulicEditTransactionCapture final
{
public:
    HydraulicEditTransactionCapture(QString operation, QString description = QString());

    // Call before a mutation. A missing entity is remembered as a creation.
    bool watch(const NetworkHydraulic &network, const QUuid &uuid);
    // Call after an editor has allocated a new UUID during the mutation.
    bool watchCreated(const QUuid &uuid);
    // Returns nullopt for no effective changes OR invalid capture. Check error.
    std::optional<QByteArray> finish(const NetworkHydraulic &network, QString *error = nullptr) const;

private:
    struct EntityImage final
    {
        QString type;
        std::optional<QByteArray> data;
        bool ambiguous = false; // Same UUID in multiple entity collections.
    };

    static EntityImage image(const NetworkHydraulic &network, const QUuid &uuid);

    QUuid transaction_id_;
    QString operation_;
    QString description_;
    QHash<QUuid, EntityImage> before_;
    bool valid_ = true;
};

#endif // HYDRAULIC_EDIT_TRANSACTION_CAPTURE_H
