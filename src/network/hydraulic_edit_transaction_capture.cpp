#include "network/hydraulic_edit_transaction_capture.h"
#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/uuid.h>

#include <utility>

namespace
{
template<typename T>
int findEncoded(const QList<T> &entities, const QUuid &uuid,
                const QString &type, QString &foundType,
                std::optional<QByteArray> &foundBytes)
{
    int matches = 0;
    for (const T &entity : entities)
    {
        if (entity.uuid != uuid)
            continue;
        ++matches;
        foundType = type;
        foundBytes = aowis::model::ModelCodec<T>::encode(entity);
    }
    return matches;
}
}

HydraulicEditTransactionCapture::HydraulicEditTransactionCapture(QString operation, QString description)
    : transaction_id_(createUuidV7()), operation_(std::move(operation)), description_(std::move(description))
{}

HydraulicEditTransactionCapture::EntityImage HydraulicEditTransactionCapture::image(
    const NetworkHydraulic &network, const QUuid &uuid)
{
    EntityImage result;
    if (!network.uuid.isNull() && uuid == network.uuid)
    {
        result.type = QString::fromLatin1(aowis::model::hydraulicRootEditType);
        result.data = aowis::model::hydraulicRootImage(network);
        return result;
    }
    int matches = 0;
#define FIND_HYDRAULIC_ENTITY(name, model_type, storage_kind, ordinal) \
    matches += findEncoded(network.name, uuid, QStringLiteral(#model_type), result.type, result.data);
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(FIND_HYDRAULIC_ENTITY)
#undef FIND_HYDRAULIC_ENTITY
    result.ambiguous = matches > 1;
    return result;
}

bool HydraulicEditTransactionCapture::watch(const NetworkHydraulic &network, const QUuid &uuid)
{
    if (uuid.isNull())
    {
        this->valid_ = false;
        return false;
    }
    if (!this->before_.contains(uuid))
    {
        const EntityImage before = image(network, uuid);
        if (before.ambiguous)
        {
            this->valid_ = false;
            return false;
        }
        this->before_.insert(uuid, before);
    }
    return true;
}

bool HydraulicEditTransactionCapture::watchCreated(const QUuid &uuid)
{
    if (uuid.isNull() || this->before_.contains(uuid))
    {
        this->valid_ = false;
        return false;
    }
    this->before_.insert(uuid, EntityImage{});
    return true;
}

std::optional<QByteArray> HydraulicEditTransactionCapture::finish(
    const NetworkHydraulic &network, QString *error) const
{
    if (error)
        error->clear();
    if (!this->valid_)
    {
        if (error)
            *error = QStringLiteral("Invalid, duplicate or ambiguous hydraulic entity watch");
        return std::nullopt;
    }

    aowis::model::ModelEditTransaction transaction;
    transaction.transactionId = this->transaction_id_;
    transaction.operation = this->operation_;
    transaction.description = this->description_;
    for (QHash<QUuid, EntityImage>::const_iterator iterator = this->before_.cbegin();
         iterator != this->before_.cend(); ++iterator)
    {
        const EntityImage after = image(network, iterator.key());
        const EntityImage &before = iterator.value();
        if (before.ambiguous || after.ambiguous)
        {
            if (error)
                *error = QStringLiteral("Duplicate entity UUID in hydraulic collections");
            return std::nullopt;
        }
        // Never silently accept an invalid image as a no-op. An entity
        // encoding failure must block capture even if nothing visibly changed.
        if ((before.data.has_value() && before.data->isEmpty()) ||
            (after.data.has_value() && after.data->isEmpty()) ||
            (before.data.has_value() && after.data.has_value() && before.type != after.type))
        {
            if (error)
                *error = QStringLiteral("Unable to capture consistent entity image");
            return std::nullopt;
        }
        if (before.type == after.type && before.data == after.data)
            continue;
        aowis::model::ModelEntityEdit edit;
        edit.entityId = iterator.key();
        edit.entityType = before.data.has_value() ? before.type : after.type;
        edit.before = before.data;
        edit.after = after.data;
        transaction.edits.append(std::move(edit));
    }
    if (transaction.edits.isEmpty())
        return std::nullopt;
    const QByteArray encoded = aowis::model::ModelEditTransactionCodec::encode(transaction, error);
    if (encoded.isEmpty())
        return std::nullopt;
    return encoded;
}
