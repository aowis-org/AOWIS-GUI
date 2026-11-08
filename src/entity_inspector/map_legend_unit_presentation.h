#pragma once

#include "config/unit_presentation_context.h"
#include "config/unit_table_presentation.h"
#include "entity_inspector/map_legend_metric_descriptors.h"

#include <aowis/model/units/conversion.h>

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

// Symbology ranges remain canonical; convert only the displayed ramp numbers.
namespace MapLegendUnitPresentation
{
struct Range
{
    double minimum = 0.0;
    double maximum = 0.0;
    QString unit;
};

inline QString label(const MapLegendMetrics::Descriptor &descriptor)
{
    return QCoreApplication::translate("LiveUiText", descriptor.label);
}

inline QString canonicalSymbol(const MapLegendMetrics::Descriptor &descriptor)
{
    if (descriptor.quantity.empty()) return {};
    const std::string_view canonical = aowis::units::canonicalUnit(descriptor.quantity);
    return UnitTablePresentation::unitSymbol(
        QString::fromUtf8(canonical.data(), static_cast<int>(canonical.size())));
}

inline Range range(const UnitPresentationContext &context,
                   const MapLegendMetrics::Descriptor &descriptor,
                   double minimum, double maximum)
{
    Range result{minimum, maximum, canonicalSymbol(descriptor)};
    if (descriptor.quantity.empty()) return result;

    const std::string_view source = aowis::units::canonicalUnit(descriptor.quantity);
    QString chosen = UnitTablePresentation::selectedUnit(context, descriptor.quantity);
    if (descriptor.quantity == AOWIS_QUANTITY("length")
        && std::isfinite(minimum) && std::isfinite(maximum))
    {
        const double largest = std::max(std::abs(minimum), std::abs(maximum));
        if (chosen == QStringLiteral("m") && largest >= 1000.0)
            chosen = QStringLiteral("km");
        else if (chosen == QStringLiteral("ft") && largest >= 1609.344)
            chosen = QStringLiteral("mi");
    }
    const std::string target = chosen.toStdString();
    const std::optional<double> lower = aowis::units::convert(
        descriptor.quantity, minimum, source, target, context.specificGravity());
    const std::optional<double> upper = aowis::units::convert(
        descriptor.quantity, maximum, source, target, context.specificGravity());
    if (!lower || !upper) return result;

    result.minimum = *lower;
    result.maximum = *upper;
    result.unit = UnitTablePresentation::unitSymbol(chosen);
    return result;
}

inline QString unit(const UnitPresentationContext &context,
                    const MapLegendMetrics::Descriptor &descriptor)
{
    if (descriptor.quantity.empty()) return {};
    return UnitTablePresentation::unitSymbol(
        UnitTablePresentation::selectedUnit(context, descriptor.quantity));
}
}
