#include "map/bgfx/map_bgfx_legend_content.h"

#include "config/unit_profile_manager.h"
#include "entity_inspector/map_legend_metric_descriptors.h"
#include "entity_inspector/map_legend_unit_presentation.h"
#include "network/hydraulic_data.h"
#include "network/network_symbology_rendering.h"

#include <QCoreApplication>

#include <array>
#include <cmath>

namespace
{
// EntityMapLegendHud's combo contents and palette menu order.
const std::array<VisualNode, 13> node_visuals = {{
    VisualNode::None,
    VisualNode::Elevation,
    VisualNode::BaseDemand,
    VisualNode::TotalDemand,
    VisualNode::DemandDeficit,
    VisualNode::EmitterFlow,
    VisualNode::Leakage,
    VisualNode::Head,
    VisualNode::Pressure,
    VisualNode::WaterAge,
    VisualNode::Chlorine,
    VisualNode::RiverWater,
    VisualNode::LakeWater
}};

const std::array<VisualLink, 12> link_visuals = {{
    VisualLink::None,
    VisualLink::Diameter,
    VisualLink::Length,
    VisualLink::Roughness,
    VisualLink::FlowRate,
    VisualLink::Velocity,
    VisualLink::HeadLoss,
    VisualLink::Leakage,
    VisualLink::WaterAge,
    VisualLink::Chlorine,
    VisualLink::RiverWater,
    VisualLink::LakeWater
}};

const std::array<VisualHeatmap, 13> heatmap_visuals = {{
    VisualHeatmap::None,
    VisualHeatmap::Elevation,
    VisualHeatmap::BaseDemand,
    VisualHeatmap::TotalDemand,
    VisualHeatmap::DemandDeficit,
    VisualHeatmap::EmitterFlow,
    VisualHeatmap::Leakage,
    VisualHeatmap::Head,
    VisualHeatmap::Pressure,
    VisualHeatmap::WaterAge,
    VisualHeatmap::Chlorine,
    VisualHeatmap::RiverWater,
    VisualHeatmap::LakeWater
}};

const std::array<NetworkSymbologyPalette, NetworkSymbologyPaletteCount> palette_choices = {{
    NetworkSymbologyPalette::Viridis,
    NetworkSymbologyPalette::Cividis,
    NetworkSymbologyPalette::Plasma,
    NetworkSymbologyPalette::Inferno,
    NetworkSymbologyPalette::Magma,
    NetworkSymbologyPalette::Batlow,
    NetworkSymbologyPalette::Turbo,
    NetworkSymbologyPalette::CoolWarm,
    NetworkSymbologyPalette::RedBlue
}};

QString legendGroupTitle(const QString &scope, const QString &metric, const QString &unit)
{
    if (unit.isEmpty())
        return QStringLiteral("%1 · %2").arg(scope, metric);
    return QStringLiteral("%1 · %2 [%3]").arg(scope, metric, unit);
}

MapLegendMetrics::RoughnessStyle roughnessStyle(HydraulicHeadlossFormula formula)
{
    switch (formula)
    {
    case HydraulicHeadlossFormula::HazenWilliams:
        return MapLegendMetrics::RoughnessStyle::HazenWilliams;
    case HydraulicHeadlossFormula::DarcyWeisbach:
        return MapLegendMetrics::RoughnessStyle::DarcyWeisbach;
    case HydraulicHeadlossFormula::ChezyManning:
        return MapLegendMetrics::RoughnessStyle::ChezyManning;
    }
    return MapLegendMetrics::RoughnessStyle::HazenWilliams;
}

bool paletteIsDiverging(NetworkSymbologyPalette palette)
{
    return palette == NetworkSymbologyPalette::CoolWarm
        || palette == NetworkSymbologyPalette::RedBlue;
}

// The palette's stops in ramp order; flipped reverses them, which is what
// networkSymbologyInterpolatedRampColor(1 - fraction) does per sample.
QVector<QRgb> paletteStops(NetworkSymbologyPalette palette, bool flipped)
{
    const std::array<QColor, NetworkSymbologyRampColorCount> &colors =
        networkSymbologyPaletteColors(palette);
    QVector<QRgb> stops;
    stops.reserve(qsizetype(colors.size()));
    for (std::size_t index = 0; index < colors.size(); ++index)
    {
        const std::size_t source = flipped ? colors.size() - 1 - index : index;
        stops.append(colors.at(source).rgb());
    }
    return stops;
}

void setRamp(MapBgfxHud::LegendRamp *ramp, const MapLegendUnitPresentation::Range &displayed,
             NetworkSymbologyPalette palette, bool flipped,
             NetworkSymbologyPalette default_palette)
{
    ramp->minimum = displayed.minimum;
    ramp->maximum = displayed.maximum;
    ramp->unit = displayed.unit;
    ramp->colors = paletteStops(palette, flipped);
    ramp->palette = static_cast<int>(palette);
    ramp->flipped = flipped;
    ramp->default_palette = static_cast<int>(default_palette);
    ramp->default_flipped = false;
}
}

MapBgfxHud::Legend mapBgfxLegendContent(const HydraulicData &hydraulic_data,
                                        const NetworkSymbologySettings &settings,
                                        const NetworkSymbologyRanges &ranges)
{
    MapBgfxHud::Legend legend;
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();

    // Node (EntityMapLegendHud::updateNodeLegend()).
    {
        MapBgfxHud::LegendGroup &group = legend.groups[0];
        for (const VisualNode visual : node_visuals)
        {
            const MapLegendMetrics::Descriptor &option = MapLegendMetrics::node(visual);
            if (visual == settings.visual_node)
                group.current = int(group.items.size());
            group.items.append(legendGroupTitle(QCoreApplication::translate("MapBgfxLegendContent", "Node"),
                                                MapLegendUnitPresentation::label(option),
                                                MapLegendUnitPresentation::unit(context, option)));
            group.values.append(static_cast<int>(visual));
        }
        group.ramp.visible = settings.visual_node != VisualNode::None;
        const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
            context, MapLegendMetrics::node(settings.visual_node),
            ranges.node_minimum, ranges.node_maximum);
        setRamp(&group.ramp, displayed, settings.node_palette, settings.node_palette_flipped,
                NetworkSymbologyDefaultNodePalette);
    }

    // Link (EntityMapLegendHud::updateLinkLegend()): Roughness follows the
    // head-loss formula; the selected entry shows the ramp's unit.
    {
        MapBgfxHud::LegendGroup &group = legend.groups[1];
        const MapLegendMetrics::RoughnessStyle roughness = roughnessStyle(
            hydraulic_data.networkHydraulic().options_hydraulic.headloss_formula);
        const MapLegendMetrics::Descriptor &descriptor =
            MapLegendMetrics::link(settings.visual_link, roughness);
        const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
            context, descriptor, ranges.link_minimum, ranges.link_maximum);
        for (const VisualLink visual : link_visuals)
        {
            const MapLegendMetrics::Descriptor &option = MapLegendMetrics::link(visual, roughness);
            const bool selected = visual == settings.visual_link;
            if (selected)
                group.current = int(group.items.size());
            // The active selection may use km/mi for readability even when
            // the profile uses metres/feet; the combo must match the ramp.
            const QString unit = selected && settings.visual_link != VisualLink::None
                ? displayed.unit
                : MapLegendUnitPresentation::unit(context, option);
            group.items.append(legendGroupTitle(QCoreApplication::translate("MapBgfxLegendContent", "Link"),
                                                MapLegendUnitPresentation::label(option), unit));
            group.values.append(static_cast<int>(visual));
        }
        group.ramp.visible = settings.visual_link != VisualLink::None;
        setRamp(&group.ramp, displayed, settings.link_palette, settings.link_palette_flipped,
                NetworkSymbologyDefaultLinkPalette);
    }

    // Heatmap (EntityMapLegendHud::updateHeatmapLegend()).
    {
        MapBgfxHud::LegendGroup &group = legend.groups[2];
        for (const VisualHeatmap visual : heatmap_visuals)
        {
            const MapLegendMetrics::Descriptor &option = MapLegendMetrics::heatmap(visual);
            if (visual == settings.visual_heatmap)
                group.current = int(group.items.size());
            group.items.append(legendGroupTitle(QCoreApplication::translate("MapBgfxLegendContent", "Heatmap"),
                                                MapLegendUnitPresentation::label(option),
                                                MapLegendUnitPresentation::unit(context, option)));
            group.values.append(static_cast<int>(visual));
        }
        group.ramp.visible = settings.visual_heatmap != VisualHeatmap::None;
        const MapLegendUnitPresentation::Range displayed = MapLegendUnitPresentation::range(
            context, MapLegendMetrics::heatmap(settings.visual_heatmap),
            ranges.heatmap_minimum, ranges.heatmap_maximum);
        setRamp(&group.ramp, displayed, settings.heatmap_palette,
                settings.heatmap_palette_flipped, NetworkSymbologyDefaultHeatmapPalette);
    }

    for (const NetworkSymbologyPalette palette : palette_choices)
    {
        MapBgfxHud::LegendPaletteChoice choice;
        choice.value = static_cast<int>(palette);
        choice.name = networkSymbologyPaletteName(palette);
        choice.colors = paletteStops(palette, false);
        choice.diverging = paletteIsDiverging(palette);
        legend.palettes.append(choice);
    }
    return legend;
}

MapBgfxLabelValueFormat mapBgfxLabelValueFormat(const HydraulicData &hydraulic_data,
                                                const NetworkSymbologySettings &settings,
                                                const NetworkSymbologyRanges &ranges,
                                                bool links)
{
    MapBgfxLabelValueFormat format;
    if (links ? settings.visual_link == VisualLink::None
              : settings.visual_node == VisualNode::None)
        return format;
    const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
    const MapLegendMetrics::Descriptor &descriptor = links
        ? MapLegendMetrics::link(settings.visual_link,
              roughnessStyle(hydraulic_data.networkHydraulic().options_hydraulic.headloss_formula))
        : MapLegendMetrics::node(settings.visual_node);
    const double minimum = links ? ranges.link_minimum : ranges.node_minimum;
    const double maximum = links ? ranges.link_maximum : ranges.node_maximum;
    // Two distinct canonical points through the legend's own conversion
    // give the affine map: the unit is chosen from the ramp range exactly
    // as for the ramp (a uniform range uses minimum + 1 as second point,
    // which picks the same unit).
    const double low = std::isfinite(minimum) ? minimum : 0.0;
    const double high = std::isfinite(maximum) && maximum != low ? maximum : low + 1.0;
    const MapLegendUnitPresentation::Range displayed =
        MapLegendUnitPresentation::range(context, descriptor, low, high);
    if (!std::isfinite(displayed.minimum) || !std::isfinite(displayed.maximum))
        return format;
    format.scale = (displayed.maximum - displayed.minimum) / (high - low);
    format.offset = displayed.minimum - format.scale * low;
    format.valid = std::isfinite(format.scale) && std::isfinite(format.offset);
    return format;
}
