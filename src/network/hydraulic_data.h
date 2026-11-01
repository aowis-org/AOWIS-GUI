#ifndef HYDRAULIC_DATA_H
#define HYDRAULIC_DATA_H

#include <optional>

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUuid>

#include <QDebug>

#include <aowis/model/entity.h>
#include <aowis/model/gis.h>
#include <aowis/model/hydraulic/network_hydraulic.h>
#include <aowis/model/hydraulic/hydraulic_simulation_results.h>
#include <aowis/model/hydraulic/water_quality_simulation_results.h>


#include <aowis/epanet/dummy/dummy_networks.h>
#include <aowis/epanet/dummy/dummy_marburg_network_generator.h>

#include "common/_enums_structs.h"
#include "network/hydraulic_network_editor.h"
#include "network/hydraulic_edit_transaction_capture.h"
#include "network/network_render_snapshot.h"
#include "network/network_symbology.h"

struct HydraulicNodeCommonData
{
    QString id;
    QUuid uuid;
    CoordinateWGS84 coordinate_wgs84;
    HydraulicEntityMetadata metadata;
};

struct HydraulicLinkCommonData
{
    QString id;
    QUuid uuid;
    HydraulicEntityMetadata metadata;
};

class HydraulicData : public QObject
{
    Q_OBJECT
public:
    explicit HydraulicData(QObject *parent = nullptr);

    const NetworkHydraulic &networkHydraulic() const;
    // Disabled until a personal working copy is bound. Captured events are not
    // durable until a future working-copy controller acknowledges persistence.
    void setLogicalEditCaptureEnabled(bool enabled);
    bool logicalEditCaptureEnabled() const;
    void replaceNetworkHydraulic(
        NetworkHydraulic network,
        QList<WaterQualitySolverOptions> quality_run_options);
    const QList<WaterQualitySolverOptions> &simulationQualityRunOptions() const;
    void setSimulationHeadlossFormula(HydraulicHeadlossFormula formula);
    QUuid sourceTraceOriginNodeUuid() const;
    bool setSourceTraceOriginNodeUuid(const QUuid &uuid);

    bool hasSimulationResults() const;
    const std::optional<HydraulicSimulationResultTimeline> &simulationResultTimeline() const;
    bool hasWaterQualitySimulationResults() const;
    const QList<WaterQualitySimulationResultTimeline> &waterQualitySimulationResultTimelines() const;
    const std::optional<WaterQualitySimulationResultTimeline> &waterQualitySimulationResultTimeline() const;
    const WaterQualitySimulationResult *currentWaterQualitySimulationResult() const;
    const WaterQualitySimulationResult *currentWaterQualitySimulationResult(
        WaterQualityAnalysisType analysis) const;
    const HydraulicSimulationStatus *simulationStatus() const;
    const HydraulicSimulationStatus *waterQualitySimulationStatus() const;
    InfrastructureEntity simulationErrorEntityType() const;
    QUuid simulationErrorEntityUuid() const;
    QHash<QUuid, InfrastructureEntity> simulationErrorEntities() const;
    bool simulationDiagnosticsStale() const;
    bool simulationDiagnosticEntityStale(const QUuid &uuid) const;
    const QSet<QUuid> &simulationStaleDiagnosticEntityUuids() const;
    const HydraulicSimulationResult *currentSimulationResult() const;
    int currentSimulationResultIndex() const;
    void setSimulationResultTimeline(const HydraulicSimulationResultTimeline &result_timeline);
    void setWaterQualitySimulationResultTimelines(
        const QList<WaterQualitySimulationResultTimeline> &result_timelines);
    void setWaterQualitySimulationResultTimeline(
        const WaterQualitySimulationResultTimeline &result_timeline);
    void clearSimulationResultTimeline();
    void clearWaterQualitySimulationResultTimeline();
    bool setCurrentSimulationResultIndex(int result_index);
    const NetworkRenderSnapshot &networkRenderSnapshot() const;
    quint64 geometryRevision() const;
    quint64 visualRevision() const;
    QDateTime timeChangedLast() const;
    bool boundingBoxWgs84Valid() const;
    const CoordinateWGS84 &boundingBoxWgs84Minimum() const;
    const CoordinateWGS84 &boundingBoxWgs84Maximum() const;
    NetworkSymbologyRanges symbologyRanges(
        const NetworkSymbologySettings &settings) const;

    double nodeElevationMMinimum() const;
    double nodeElevationMMaximum() const;
    void setNodeElevationMMinimum(double node_elevation_m_minimum);
    void setNodeElevationMMaximum(double node_elevation_m_maximum);

    double nodeBaseDemandM3PerHMinimum() const;
    double nodeBaseDemandM3PerHMaximum() const;
    void setNodeBaseDemandM3PerHMinimum(double node_base_demand_m3_per_h_minimum);
    void setNodeBaseDemandM3PerHMaximum(double node_base_demand_m3_per_h_maximum);

    double nodeTotalDemandM3PerHMinimum() const;
    double nodeTotalDemandM3PerHMaximum() const;
    void setNodeTotalDemandM3PerHMinimum(double node_total_demand_m3_per_h_minimum);
    void setNodeTotalDemandM3PerHMaximum(double node_total_demand_m3_per_h_maximum);

    double nodeDemandDeficitM3PerHMinimum() const;
    double nodeDemandDeficitM3PerHMaximum() const;
    void setNodeDemandDeficitM3PerHMinimum(double node_demand_deficit_m3_per_h_minimum);
    void setNodeDemandDeficitM3PerHMaximum(double node_demand_deficit_m3_per_h_maximum);

    double nodeEmitterFlowM3PerHMinimum() const;
    double nodeEmitterFlowM3PerHMaximum() const;
    void setNodeEmitterFlowM3PerHMinimum(double node_emitter_flow_m3_per_h_minimum);
    void setNodeEmitterFlowM3PerHMaximum(double node_emitter_flow_m3_per_h_maximum);

    double nodeLeakageM3PerHMinimum() const;
    double nodeLeakageM3PerHMaximum() const;
    void setNodeLeakageM3PerHMinimum(double node_leakage_m3_per_h_minimum);
    void setNodeLeakageM3PerHMaximum(double node_leakage_m3_per_h_maximum);

    double nodeHeadMMinimum() const;
    double nodeHeadMMaximum() const;
    void setNodeHeadMMinimum(double node_head_m_minimum);
    void setNodeHeadMMaximum(double node_head_m_maximum);

    double nodePressureMMinimum() const;
    double nodePressureMMaximum() const;
    void setNodePressureMMinimum(double node_pressure_m_minimum);
    void setNodePressureMMaximum(double node_pressure_m_maximum);

    double nodeChlorineMgPerLMinimum() const;
    double nodeChlorineMgPerLMaximum() const;
    void setNodeChlorineMgPerLMinimum(double node_chlorine_mg_per_l_minimum);
    void setNodeChlorineMgPerLMaximum(double node_chlorine_mg_per_l_maximum);

    double nodeRiverWaterPercentMinimum() const;
    double nodeRiverWaterPercentMaximum() const;
    void setNodeRiverWaterPercentMinimum(double node_river_water_percent_minimum);
    void setNodeRiverWaterPercentMaximum(double node_river_water_percent_maximum);

    double nodeLakeWaterPercentMinimum() const;
    double nodeLakeWaterPercentMaximum() const;
    void setNodeLakeWaterPercentMinimum(double node_lake_water_percent_minimum);
    void setNodeLakeWaterPercentMaximum(double node_lake_water_percent_maximum);

    double linkDiameterMmMinimum() const;
    double linkDiameterMmMaximum() const;
    void setLinkDiameterMmMinimum(double link_diameter_mm_minimum);
    void setLinkDiameterMmMaximum(double link_diameter_mm_maximum);

    double linkLengthMMinimum() const;
    double linkLengthMMaximum() const;
    void setLinkLengthMMinimum(double link_length_m_minimum);
    void setLinkLengthMMaximum(double link_length_m_maximum);

    double linkRoughnessHwMinimum() const;
    double linkRoughnessHwMaximum() const;
    void setLinkRoughnessHwMinimum(double link_roughness_hw_minimum);
    void setLinkRoughnessHwMaximum(double link_roughness_hw_maximum);

    double linkRoughnessDwMmMinimum() const;
    double linkRoughnessDwMmMaximum() const;
    void setLinkRoughnessDwMmMinimum(double link_roughness_dw_mm_minimum);
    void setLinkRoughnessDwMmMaximum(double link_roughness_dw_mm_maximum);

    double linkRoughnessCmMinimum() const;
    double linkRoughnessCmMaximum() const;
    void setLinkRoughnessCmMinimum(double link_roughness_cm_minimum);
    void setLinkRoughnessCmMaximum(double link_roughness_cm_maximum);

    double linkFlowRateM3PerHMinimum() const;
    double linkFlowRateM3PerHMaximum() const;
    void setLinkFlowRateM3PerHMinimum(double link_flow_rate_m3_per_h_minimum);
    void setLinkFlowRateM3PerHMaximum(double link_flow_rate_m3_per_h_maximum);

    double linkVelocityMPerSMinimum() const;
    double linkVelocityMPerSMaximum() const;
    void setLinkVelocityMPerSMinimum(double link_velocity_m_per_s_minimum);
    void setLinkVelocityMPerSMaximum(double link_velocity_m_per_s_maximum);

    double linkHeadLossMMinimum() const;
    double linkHeadLossMMaximum() const;
    void setLinkHeadLossMMinimum(double link_head_loss_m_minimum);
    void setLinkHeadLossMMaximum(double link_head_loss_m_maximum);

    double linkLeakageM3PerHMinimum() const;
    double linkLeakageM3PerHMaximum() const;
    void setLinkLeakageM3PerHMinimum(double link_leakage_m3_per_h_minimum);
    void setLinkLeakageM3PerHMaximum(double link_leakage_m3_per_h_maximum);

    double linkChlorineMgPerLMinimum() const;
    double linkChlorineMgPerLMaximum() const;
    void setLinkChlorineMgPerLMinimum(double link_chlorine_mg_per_l_minimum);
    void setLinkChlorineMgPerLMaximum(double link_chlorine_mg_per_l_maximum);

    double linkRiverWaterPercentMinimum() const;
    double linkRiverWaterPercentMaximum() const;
    void setLinkRiverWaterPercentMinimum(double link_river_water_percent_minimum);
    void setLinkRiverWaterPercentMaximum(double link_river_water_percent_maximum);

    double linkLakeWaterPercentMinimum() const;
    double linkLakeWaterPercentMaximum() const;
    void setLinkLakeWaterPercentMinimum(double link_lake_water_percent_minimum);
    void setLinkLakeWaterPercentMaximum(double link_lake_water_percent_maximum);

    double heatmapElevationMMinimum() const;
    double heatmapElevationMMaximum() const;
    void setHeatmapElevationMMinimum(double heatmap_elevation_m_minimum);
    void setHeatmapElevationMMaximum(double heatmap_elevation_m_maximum);

    double heatmapTotalDemandM3PerHMinimum() const;
    double heatmapTotalDemandM3PerHMaximum() const;
    void setHeatmapTotalDemandM3PerHMinimum(double heatmap_total_demand_m3_per_h_minimum);
    void setHeatmapTotalDemandM3PerHMaximum(double heatmap_total_demand_m3_per_h_maximum);

    double heatmapDemandDeficitM3PerHMinimum() const;
    double heatmapDemandDeficitM3PerHMaximum() const;
    void setHeatmapDemandDeficitM3PerHMinimum(double heatmap_demand_deficit_m3_per_h_minimum);
    void setHeatmapDemandDeficitM3PerHMaximum(double heatmap_demand_deficit_m3_per_h_maximum);

    double heatmapLeakageM3PerHMinimum() const;
    double heatmapLeakageM3PerHMaximum() const;
    void setHeatmapLeakageM3PerHMinimum(double heatmap_leakage_m3_per_h_minimum);
    void setHeatmapLeakageM3PerHMaximum(double heatmap_leakage_m3_per_h_maximum);

    double heatmapHeadMMinimum() const;
    double heatmapHeadMMaximum() const;
    void setHeatmapHeadMMinimum(double heatmap_head_m_minimum);
    void setHeatmapHeadMMaximum(double heatmap_head_m_maximum);

    double heatmapPressureMMinimum() const;
    double heatmapPressureMMaximum() const;
    void setHeatmapPressureMMinimum(double heatmap_pressure_m_minimum);
    void setHeatmapPressureMMaximum(double heatmap_pressure_m_maximum);

    double heatmapChlorineMgPerLMinimum() const;
    double heatmapChlorineMgPerLMaximum() const;
    void setHeatmapChlorineMgPerLMinimum(double heatmap_chlorine_mg_per_l_minimum);
    void setHeatmapChlorineMgPerLMaximum(double heatmap_chlorine_mg_per_l_maximum);

    double heatmapRiverWaterPercentMinimum() const;
    double heatmapRiverWaterPercentMaximum() const;
    void setHeatmapRiverWaterPercentMinimum(double heatmap_river_water_percent_minimum);
    void setHeatmapRiverWaterPercentMaximum(double heatmap_river_water_percent_maximum);

    double heatmapLakeWaterPercentMinimum() const;
    double heatmapLakeWaterPercentMaximum() const;
    void setHeatmapLakeWaterPercentMinimum(double heatmap_lake_water_percent_minimum);
    void setHeatmapLakeWaterPercentMaximum(double heatmap_lake_water_percent_maximum);

    std::optional<HydraulicNodeJunction> junction(const QUuid &uuid) const;
    std::optional<HydraulicNodeReservoir> reservoir(const QUuid &uuid) const;
    std::optional<HydraulicNodeTank> tank(const QUuid &uuid) const;
    std::optional<HydraulicDemandPoint> demandPoint(const QUuid &uuid) const;
    std::optional<HydraulicLinkPipe> pipe(const QUuid &uuid) const;
    std::optional<HydraulicLinkPump> pump(const QUuid &uuid) const;
    std::optional<HydraulicLinkValve> valve(const QUuid &uuid) const;
    std::optional<HydraulicNodeCommonData> nodeCommonData(InfrastructureEntity entity_type,
                                                           const QUuid &uuid) const;
    std::optional<HydraulicLinkCommonData> linkCommonData(InfrastructureEntity entity_type,
                                                           const QUuid &uuid) const;
    std::optional<InfrastructureEntity> nodeEntityType(const QUuid &uuid) const;

    void setSelectedUuid(InfrastructureEntity entity_type, const QUuid &uuid);
    void requestEntityLocate(InfrastructureEntity entity_type, const QUuid &uuid);
    void requestDemandPointAttachmentSelection(const QUuid &uuid);

    // Transaction-safe hydraulic libraries. Add assigns UUIDv7 and returns the
    // new identity; updates retain that identity. Removing a referenced item
    // is refused rather than leaving dangling model references.
    QUuid addPipeMaterial(HydraulicPipeMaterial entity);
    bool updatePipeMaterial(const HydraulicPipeMaterial &entity);
    bool removePipeMaterial(const QUuid &uuid);
    QUuid addPatternTime(HydraulicPatternTime entity);
    bool updatePatternTime(const HydraulicPatternTime &entity);
    bool removePatternTime(const QUuid &uuid);
    QUuid addCurveTankVolume(HydraulicCurveTankVolume entity);
    bool updateCurveTankVolume(const HydraulicCurveTankVolume &entity);
    bool removeCurveTankVolume(const QUuid &uuid);
    QUuid addCurvePumpHead(HydraulicCurvePumpHead entity);
    bool updateCurvePumpHead(const HydraulicCurvePumpHead &entity);
    bool removeCurvePumpHead(const QUuid &uuid);
    QUuid addCurvePumpEfficiency(HydraulicCurvePumpEfficiency entity);
    bool updateCurvePumpEfficiency(const HydraulicCurvePumpEfficiency &entity);
    bool removeCurvePumpEfficiency(const QUuid &uuid);
    QUuid addCurveValveHeadloss(HydraulicCurveValveHeadloss entity);
    bool updateCurveValveHeadloss(const HydraulicCurveValveHeadloss &entity);
    bool removeCurveValveHeadloss(const QUuid &uuid);
    QUuid addCurveValveCharacteristic(HydraulicCurveValveCharacteristic entity);
    bool updateCurveValveCharacteristic(const HydraulicCurveValveCharacteristic &entity);
    bool removeCurveValveCharacteristic(const QUuid &uuid);
    QUuid addCurveGeneric(HydraulicCurveGeneric entity);
    bool updateCurveGeneric(const HydraulicCurveGeneric &entity);
    bool removeCurveGeneric(const QUuid &uuid);

    QUuid addJunction(const CoordinateWGS84 &coordinate);
    QUuid addReservoir(const CoordinateWGS84 &coordinate);
    QUuid addTank(const CoordinateWGS84 &coordinate);
    QUuid addDemandPoint(const CoordinateWGS84 &coordinate);

    QUuid addPipe(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                  const QList<CoordinateWGS84> &intermediate_vertices);
    QUuid addPump(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                  const CoordinateWGS84 &center_coordinate);
    QUuid addValve(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                   const CoordinateWGS84 &center_coordinate);

    bool setNodeId(const QUuid &uuid, const QString &id);
    bool setNodeModelRole(const QUuid &uuid, EntityModelRole model_role);
    bool setNodeDateAdded(const QUuid &uuid, const std::optional<QDate> &date_added);
    bool setNodeDateInstalled(const QUuid &uuid, const std::optional<QDate> &date_installed);
    bool setNodeEnabled(const QUuid &uuid, bool enabled);
    bool setNodeCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate);
    bool setDemandPointId(const QUuid &uuid, const QString &id);
    bool setDemandPointModelRole(const QUuid &uuid, EntityModelRole model_role);
    bool setDemandPointDateAdded(const QUuid &uuid, const std::optional<QDate> &date_added);
    bool setDemandPointDateInstalled(const QUuid &uuid, const std::optional<QDate> &date_installed);
    bool setDemandPointEnabled(const QUuid &uuid, bool enabled);
    bool setDemandPointDescription(const QUuid &uuid, const QString &description);
    bool setDemandPointComment(const QUuid &uuid, const QString &comment);
    bool setDemandPointTags(const QUuid &uuid, const QStringList &tags);
    bool setDemandPointCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate);
    bool addDemandPointDemand(const QUuid &uuid, const HydraulicDemand &demand);
    bool removeDemandPointDemand(const QUuid &uuid, int demand_index);
    bool setDemandPointDemandCategoryName(const QUuid &uuid, int demand_index,
                                          const QString &category_name);
    bool setDemandPointDemandBaseDemandM3PerH(const QUuid &uuid, int demand_index,
                                              double base_demand_m3_per_h);
    bool setDemandPointDemandPatternMode(const QUuid &uuid, int demand_index,
                                         HydraulicTimePatternMode pattern_mode);
    bool setDemandPointDemandPatternUuid(const QUuid &uuid, int demand_index,
                                         const QUuid &pattern_uuid);
    bool setDemandPointDemandSourceMethod(const QUuid &uuid, int demand_index,
                                          HydraulicDemandSourceMethod source_method);
    bool setDemandPointDemandNote(const QUuid &uuid, int demand_index, const QString &note);
    bool addDemandPointMeter(const QUuid &uuid);
    bool setDemandPointMeter(const QUuid &uuid, const WaterMeter &meter);
    bool removeDemandPointMeter(const QUuid &uuid);
    bool attachDemandPointToPipe(const QUuid &uuid, const QUuid &pipe_uuid,
                                 double pipe_position);
    bool setDemandPointPipeAllocationMode(
        const QUuid &uuid, HydraulicDemandPointPipeAllocationMode allocation_mode);
    bool setDemandPointPipeAssignedJunction(const QUuid &uuid, const QUuid &junction_uuid);
    bool attachDemandPointToJunction(const QUuid &uuid, const QUuid &junction_uuid);
    bool clearDemandPointAttachment(const QUuid &uuid);

    bool setLinkId(const QUuid &uuid, const QString &id);
    bool setLinkModelRole(const QUuid &uuid, EntityModelRole model_role);
    bool setLinkDateAdded(const QUuid &uuid, const std::optional<QDate> &date_added);
    bool setLinkDateInstalled(const QUuid &uuid, const std::optional<QDate> &date_installed);
    bool setLinkEnabled(const QUuid &uuid, bool enabled);

    bool setJunctionElevationInputType(const QUuid &uuid,
                                        HydraulicNodeElevationInputType input_type);
    bool setJunctionElevationM(const QUuid &uuid, double elevation_m);
    bool setJunctionTerrainElevationM(const QUuid &uuid, double terrain_elevation_m);
    bool setJunctionElevationOffsetM(const QUuid &uuid, double elevation_offset_m);
    bool addJunctionDemand(const QUuid &uuid, const HydraulicDemand &demand);
    bool removeJunctionDemand(const QUuid &uuid, int demand_index);
    bool setJunctionDemandCategoryName(const QUuid &uuid, int demand_index,
                                       const QString &category_name);
    bool setJunctionDemandBaseDemandM3PerH(const QUuid &uuid, int demand_index,
                                           double base_demand_m3_per_h);
    bool setJunctionDemandPatternMode(const QUuid &uuid, int demand_index,
                                      HydraulicTimePatternMode pattern_mode);
    bool setJunctionDemandPatternUuid(const QUuid &uuid, int demand_index,
                                      const QUuid &pattern_uuid);
    bool setJunctionDemandSourceMethod(const QUuid &uuid, int demand_index,
                                       HydraulicDemandSourceMethod source_method);
    bool setJunctionDemandNote(const QUuid &uuid, int demand_index, const QString &note);
    bool setJunctionEmitterCoefficient(const QUuid &uuid, double coefficient);
    bool setJunctionEmitterPressureExponent(const QUuid &uuid, double pressure_exponent);

    bool setNodeInitialChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l);
    bool setNodeInitialWaterAgeH(const QUuid &uuid, double value_h);
    bool setNodeQualitySourceType(const QUuid &uuid, HydraulicNodeQualitySourceType source_type);
    bool setNodeQualitySourceChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l);
    bool setNodeQualitySourceMassFlowMgPerMin(const QUuid &uuid, double value_mg_per_min);
    bool setNodeQualitySourcePatternUuid(const QUuid &uuid, const QUuid &pattern_uuid);

    bool setReservoirHeadInputType(const QUuid &uuid,
                                   HydraulicNodeElevationInputType input_type);
    bool setReservoirHeadM(const QUuid &uuid, double hydraulic_head_m);
    bool setReservoirTerrainElevationM(const QUuid &uuid, double terrain_elevation_m);
    bool setReservoirHeadOffsetM(const QUuid &uuid, double hydraulic_head_offset_m);
    bool setReservoirHeadPatternMode(const QUuid &uuid, HydraulicTimePatternMode pattern_mode);
    bool setReservoirHeadPatternUuid(const QUuid &uuid, const QUuid &pattern_uuid);

    bool setTankElevationInputType(const QUuid &uuid,
                                   HydraulicNodeTankElevationInputType input_type);
    bool setTankBottomElevationM(const QUuid &uuid, double bottom_elevation_m);
    bool setTankTerrainElevationM(const QUuid &uuid, double terrain_elevation_m);
    bool setTankBottomOffsetM(const QUuid &uuid, double bottom_offset_m);
    bool setTankWaterLevelInitialM(const QUuid &uuid, double water_level_initial_m);
    bool setTankWaterLevelMinimumM(const QUuid &uuid, double water_level_minimum_m);
    bool setTankWaterLevelMaximumM(const QUuid &uuid, double water_level_maximum_m);
    bool setTankGeometryInputType(const QUuid &uuid,
                                  HydraulicNodeTankGeometryInputType input_type);
    bool setTankDiameterM(const QUuid &uuid, double diameter_m);
    bool setTankCrossSectionAreaM2(const QUuid &uuid, double cross_section_area_m2);
    bool setTankVolumeAtMaximumLevelM3(const QUuid &uuid,
                                       double volume_at_maximum_level_m3);
    bool setTankMinimumVolumeM3(const QUuid &uuid, double minimum_volume_m3);
    bool setTankVolumeCurveUuid(const QUuid &uuid, const QUuid &volume_curve_uuid);
    bool setTankCanOverflow(const QUuid &uuid, bool can_overflow);
    bool setTankQualityMixingModel(const QUuid &uuid, HydraulicNodeTankMixingModel mixing_model);
    bool setTankQualityMixingFraction(const QUuid &uuid, double mixing_fraction);
    bool setTankOverrideBulkReaction(const QUuid &uuid, bool override_bulk_reaction);
    bool setTankBulkReactionCoefficient(const QUuid &uuid, double coefficient);

    bool setPipeInitialStatus(const QUuid &uuid, HydraulicLinkPipeInitialStatus initial_status);
    bool setPipeDiameterMm(const QUuid &uuid, double diameter_mm);
    bool setPipeMeasuredLengthM(const QUuid &uuid, const std::optional<double> &length_measured_m);
    bool setPipeMaterialUuid(const QUuid &uuid, const QUuid &material_uuid);
    bool setPipeRoughnessMode(const QUuid &uuid, HydraulicPipeRoughnessMode roughness_mode);
    bool setPipeRoughnessHw(const QUuid &uuid, double roughness_hazen_williams);
    bool setPipeRoughnessDwMm(const QUuid &uuid, double roughness_darcy_weisbach_mm);
    bool setPipeRoughnessCm(const QUuid &uuid, double roughness_chezy_manning);
    bool setPipeMinorLoss(const QUuid &uuid, double minor_loss_coefficient);
    bool setPipeOverrideBulkReaction(const QUuid &uuid, bool override_bulk_reaction);
    bool setPipeOverrideWallReaction(const QUuid &uuid, bool override_wall_reaction);
    bool setPipeBulkReactionCoefficient(const QUuid &uuid, double coefficient);
    bool setPipeWallReactionCoefficient(const QUuid &uuid, double coefficient);

    bool setPumpDefinitionType(const QUuid &uuid,
                               HydraulicLinkPumpDefinitionType definition_type);
    bool setPumpConstantPowerKw(const QUuid &uuid, double constant_power_kw);
    bool setPumpInitialSpeed(const QUuid &uuid, double initial_speed_ratio);
    bool setPumpInitialStatus(const QUuid &uuid,
                              HydraulicLinkPumpInitialStatus initial_status);
    bool setPumpSpeedPatternUuid(const QUuid &uuid, const QUuid &speed_pattern_uuid);
    QUuid addPumpSimpleControl(const QUuid &pump_uuid, HydraulicControlSimpleType type,
                               const QUuid &trigger_node_uuid = QUuid());
    bool setPumpSimpleControl(const QUuid &pump_uuid, const HydraulicControlSimple &control);
    bool removePumpSimpleControl(const QUuid &pump_uuid, const QUuid &control_uuid);
    bool setPumpEfficiencyInput(const QUuid &uuid,
                                HydraulicLinkPumpEfficiencyInputType input_type,
                                const QUuid &efficiency_curve_uuid);
    bool setPumpEnergyPricePerKwh(const QUuid &uuid, double energy_price_per_kw_h);
    bool setPumpEnergyPriceInput(const QUuid &uuid,
                                 HydraulicLinkPumpEnergyPriceInputType input_type,
                                 const QUuid &price_pattern_uuid);

    bool setValveType(const QUuid &uuid, HydraulicLinkValveType type);
    bool setValveSettingPressureHeadM(const QUuid &uuid, double setting_pressure_head_m);
    bool setValveSettingFlowM3PerH(const QUuid &uuid, double setting_flow_m3_per_h);
    bool setValveSettingLossCoefficient(const QUuid &uuid, double setting_loss_coefficient);
    bool setValveSettingPositionPercent(const QUuid &uuid, double setting_position_percent);
    bool setValveHeadLossCurveUuid(const QUuid &uuid, const QUuid &head_loss_curve_uuid);
    bool setValveCharacteristicCurveUuid(const QUuid &uuid, const QUuid &characteristic_curve_uuid);
    bool setValveInitialStatus(const QUuid &uuid,
                               HydraulicLinkValveInitialStatus initial_status);
    bool setValveDiameterMm(const QUuid &uuid, double diameter_mm);
    bool setValveMinorLoss(const QUuid &uuid, double minor_loss_coefficient);

    bool setPipeVertexCoordinate(const QUuid &pipe_uuid, int vertex_index,
                                 const CoordinateWGS84 &coordinate);
    bool setPipeVertices(const QUuid &pipe_uuid,
                         const QList<CoordinateWGS84> &intermediate_vertices);
    bool setPumpCenterCoordinate(const QUuid &pump_uuid, const CoordinateWGS84 &coordinate);
    bool setValveCenterCoordinate(const QUuid &valve_uuid, const CoordinateWGS84 &coordinate);
    bool applyGeometryBatch(const HydraulicGeometryBatch &batch);

    QUuid splitPipeAtVertex(const QUuid &pipe_uuid, int vertex_index, const QUuid &junction_uuid);
    bool undoPipeSplit(const QUuid &first_pipe_uuid, const QUuid &second_pipe_uuid,
                       const QUuid &junction_uuid);

    bool deleteJunction(const QUuid &uuid);
    bool deleteReservoir(const QUuid &uuid);
    bool deleteTank(const QUuid &uuid);
    bool deleteDemandPoint(const QUuid &uuid);
    bool deletePipe(const QUuid &uuid);
    bool deletePump(const QUuid &uuid);
    bool deleteValve(const QUuid &uuid);

private:
    class EditScope;

    enum class NetworkChange
    {
        Visual,
        Geometry
    };

    std::optional<InfrastructureEntity> linkEntityType(const QUuid &uuid) const;
    void markNetworkChanged(NetworkChange change, const QUuid &edited_entity_uuid = QUuid());
    void markSimulationResultTimelineStale(const QUuid &edited_entity_uuid = QUuid());
    void markSimulationDiagnosticEntityStale(const QUuid &uuid);
    bool emitNodeChangedIfSuccessful(const QUuid &uuid, bool successful, NetworkChange change = NetworkChange::Visual);
    bool emitLinkChangedIfSuccessful(const QUuid &uuid, bool successful, NetworkChange change = NetworkChange::Visual);
    void emitConnectedPipeChanges(const QUuid &node_uuid);
    bool extendBoundingBoxWgs84(const CoordinateWGS84 &coordinate);
    void updateBoundingBoxWgs84(const CoordinateWGS84 &coordinate_previous,
                                const CoordinateWGS84 &coordinate);
    void rebuildBoundingBoxWgs84();
    void rebuildSymbologyMinMaxValues();
    void rebuildNetworkRenderSnapshot() const;
    // After an ID change: the render snapshot's IDs follow (map labels).
    void refreshNetworkRenderSnapshotIds();
    void publishCapturedEdit(const HydraulicEditTransactionCapture &capture);

    bool logical_edit_capture_enabled = false;
    NetworkHydraulic network_hydraulic;
    QList<WaterQualitySolverOptions> simulation_quality_run_options;
    QUuid source_trace_origin_node_uuid;
    HydraulicNetworkEditor network_editor;
    std::optional<HydraulicSimulationResultTimeline> simulation_result_timeline;
    QList<WaterQualitySimulationResultTimeline> water_quality_simulation_result_timelines;
    std::optional<WaterQualitySimulationResultTimeline> water_quality_simulation_result_timeline;
    int current_simulation_result_index = -1;
    bool simulation_result_timeline_stale = false;
    QSet<QUuid> simulation_stale_diagnostic_entity_uuids;

    quint64 geometry_revision = 0;
    quint64 visual_revision = 0;
    QDateTime time_changed_last;
    CoordinateWGS84 bounding_box_wgs84_minimum{};
    CoordinateWGS84 bounding_box_wgs84_maximum{};
    bool bounding_box_wgs84_valid = false;

    double node_elevation_m_minimum = 0.0;
    double node_elevation_m_maximum = 0.0;
    double node_base_demand_m3_per_h_minimum = 0.0;
    double node_base_demand_m3_per_h_maximum = 0.0;
    double node_total_demand_m3_per_h_minimum = 0.0;
    double node_total_demand_m3_per_h_maximum = 0.0;
    double node_demand_deficit_m3_per_h_minimum = 0.0;
    double node_demand_deficit_m3_per_h_maximum = 0.0;
    double node_emitter_flow_m3_per_h_minimum = 0.0;
    double node_emitter_flow_m3_per_h_maximum = 0.0;
    double node_leakage_m3_per_h_minimum = 0.0;
    double node_leakage_m3_per_h_maximum = 0.0;
    double node_head_m_minimum = 0.0;
    double node_head_m_maximum = 0.0;
    double node_pressure_m_minimum = 0.0;
    double node_pressure_m_maximum = 0.0;
    double node_chlorine_mg_per_l_minimum = 0.0;
    double node_chlorine_mg_per_l_maximum = 0.0;
    double node_river_water_percent_minimum = 0.0;
    double node_river_water_percent_maximum = 0.0;
    double node_lake_water_percent_minimum = 0.0;
    double node_lake_water_percent_maximum = 0.0;

    double link_diameter_mm_minimum = 0.0;
    double link_diameter_mm_maximum = 0.0;
    double link_length_m_minimum = 0.0;
    double link_length_m_maximum = 0.0;
    double link_roughness_hw_minimum = 0.0;
    double link_roughness_hw_maximum = 0.0;
    double link_roughness_dw_mm_minimum = 0.0;
    double link_roughness_dw_mm_maximum = 0.0;
    double link_roughness_cm_minimum = 0.0;
    double link_roughness_cm_maximum = 0.0;
    double link_flow_rate_m3_per_h_minimum = 0.0;
    double link_flow_rate_m3_per_h_maximum = 0.0;
    double link_velocity_m_per_s_minimum = 0.0;
    double link_velocity_m_per_s_maximum = 0.0;
    double link_head_loss_m_minimum = 0.0;
    double link_head_loss_m_maximum = 0.0;
    double link_leakage_m3_per_h_minimum = 0.0;
    double link_leakage_m3_per_h_maximum = 0.0;
    double link_chlorine_mg_per_l_minimum = 0.0;
    double link_chlorine_mg_per_l_maximum = 0.0;
    double link_river_water_percent_minimum = 0.0;
    double link_river_water_percent_maximum = 0.0;
    double link_lake_water_percent_minimum = 0.0;
    double link_lake_water_percent_maximum = 0.0;

    double heatmap_elevation_m_minimum = 0.0;
    double heatmap_elevation_m_maximum = 0.0;
    double heatmap_total_demand_m3_per_h_minimum = 0.0;
    double heatmap_total_demand_m3_per_h_maximum = 0.0;
    double heatmap_demand_deficit_m3_per_h_minimum = 0.0;
    double heatmap_demand_deficit_m3_per_h_maximum = 0.0;
    double heatmap_leakage_m3_per_h_minimum = 0.0;
    double heatmap_leakage_m3_per_h_maximum = 0.0;
    double heatmap_head_m_minimum = 0.0;
    double heatmap_head_m_maximum = 0.0;
    double heatmap_pressure_m_minimum = 0.0;
    double heatmap_pressure_m_maximum = 0.0;
    double heatmap_chlorine_mg_per_l_minimum = 0.0;
    double heatmap_chlorine_mg_per_l_maximum = 0.0;
    double heatmap_river_water_percent_minimum = 0.0;
    double heatmap_river_water_percent_maximum = 0.0;
    double heatmap_lake_water_percent_minimum = 0.0;
    double heatmap_lake_water_percent_maximum = 0.0;

    mutable NetworkRenderSnapshot network_render_snapshot;

signals:
    // Canonical MODEL event bytes. One emission = one logical editing action.
    // Not a durability acknowledgement; the working-copy controller owns that.
    void signalLogicalEditRecorded(const QByteArray &encoded_transaction);
    void signalLogicalEditCaptureFailed(const QString &reason);
    void signalNetworkLoaded();
    void signalWaterQualityOptionsChanged();
    void signalSimulationResultTimelineChanged(bool available);
    void signalWaterQualitySimulationResultTimelineChanged(bool available);
    void signalCurrentSimulationResultChanged(int result_index);
    void signalSimulationHeadlossFormulaChanged();
    void signalNetworkGeometryChanged(quint64 geometry_revision);
    void signalBoundingBoxWgs84Changed();
    void signalNodeChanged(InfrastructureEntity entity_type, const QUuid &uuid);
    void signalDemandPointChanged(const QUuid &uuid);
    void signalDemandPointAttachmentSelectionRequested(const QUuid &uuid);
    void signalLinkChanged(InfrastructureEntity entity_type, const QUuid &uuid);
    void signalEntityLocateRequested(InfrastructureEntity entity_type, const QUuid &uuid);
    void signalSelectedTank(const HydraulicNodeTank &tank);
    void signalSelectedReservoir(const HydraulicNodeReservoir &reservoir);
    void signalSelectedJunction(const HydraulicNodeJunction &junction);
    void signalSelectedPipe(const HydraulicLinkPipe &pipe);
    void signalSelectedPump(const HydraulicLinkPump &pump);
    void signalSelectedValve(const HydraulicLinkValve &valve);
    void signalSelectedDemandPoint(const HydraulicDemandPoint &demand_point);
};

#endif // HYDRAULIC_DATA_H
