#ifndef HYDRAULIC_NETWORK_EDITOR_H
#define HYDRAULIC_NETWORK_EDITOR_H

#include <optional>

#include <QDate>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QUuid>

#include <aowis/model/entity.h>
#include <aowis/model/gis.h>
#include <aowis/model/hydraulic/network_hydraulic.h>


struct HydraulicGeometryBatch
{
    QHash<QUuid, CoordinateWGS84> node_coordinates;
    // Only terrain-relative nodes receive a new terrain component. Absolute
    // elevations and every offset remain unchanged.
    QHash<QUuid, double> node_terrain_elevations_m;
    QHash<QUuid, CoordinateWGS84> demand_point_coordinates;
    QHash<QUuid, QList<CoordinateWGS84>> pipe_vertices;
    QHash<QUuid, CoordinateWGS84> pump_center_coordinates;
    QHash<QUuid, CoordinateWGS84> valve_center_coordinates;

    bool isEmpty() const
    {
        return this->node_coordinates.isEmpty() && this->node_terrain_elevations_m.isEmpty() &&
               this->demand_point_coordinates.isEmpty() &&
               this->pipe_vertices.isEmpty() &&
               this->pump_center_coordinates.isEmpty() &&
               this->valve_center_coordinates.isEmpty();
    }
};

struct HydraulicGeometryBatchResult
{
    bool successful = false;
    QSet<QUuid> affected_pipe_uuids;
};

class HydraulicNetworkEditor
{
public:
    explicit HydraulicNetworkEditor(NetworkHydraulic &network);

    bool hasNode(const QUuid &uuid) const;
    std::optional<CoordinateWGS84> nodeCoordinate(const QUuid &uuid) const;
    std::optional<HydraulicNodeJunction> junction(const QUuid &uuid) const;
    std::optional<HydraulicNodeReservoir> reservoir(const QUuid &uuid) const;
    std::optional<HydraulicNodeTank> tank(const QUuid &uuid) const;
    std::optional<HydraulicDemandPoint> demandPoint(const QUuid &uuid) const;
    std::optional<HydraulicLinkPipe> pipe(const QUuid &uuid) const;
    std::optional<HydraulicLinkPump> pump(const QUuid &uuid) const;
    std::optional<HydraulicLinkValve> valve(const QUuid &uuid) const;

    QUuid addJunction(const CoordinateWGS84 &coordinate,
                  std::optional<double> terrain_elevation_m = std::nullopt);
    QUuid addReservoir(const CoordinateWGS84 &coordinate,
                  std::optional<double> terrain_elevation_m = std::nullopt);
    QUuid addTank(const CoordinateWGS84 &coordinate,
                  std::optional<double> terrain_elevation_m = std::nullopt);
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
    HydraulicGeometryBatchResult applyGeometryBatch(const HydraulicGeometryBatch &batch);

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
    bool assignDemandPointToNearestPipeJunction(
        HydraulicDemandPoint &demand_point, const HydraulicLinkPipe &pipe);
    void clearDemandPointAttachmentsToPipe(const QUuid &pipe_uuid);
    void clearDemandPointAttachmentsToJunction(const QUuid &junction_uuid);
    void clearMapLabelAnchorsToNode(const QUuid &node_uuid);
    void deleteConnectedLinks(const QUuid &node_uuid);
    QString nextNodeId(const QString &prefix) const;
    QString nextLinkId(const QString &prefix) const;
    QString nextDemandPointId() const;
    QString nextWaterMeterId() const;
    QString nextSimpleControlId() const;
    double pipeLengthMeters(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                            const QList<HydraulicLinkVertex> &vertices) const;
    std::optional<double> pipeVertexPosition(const HydraulicLinkPipe &pipe,
                                             int vertex_index) const;
    void recalculateConnectedPipeLengths(const QUuid &node_uuid);

    NetworkHydraulic &network;
};

#endif // HYDRAULIC_NETWORK_EDITOR_H
