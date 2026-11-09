#ifndef ENTITY_INSPECTOR_WIDGET_H
#define ENTITY_INSPECTOR_WIDGET_H

#include <QWidget>
#include <QByteArray>
#include <QTabWidget>
#include <QScrollArea>
#include <QDialog>
#include <QPointer>
#include <QIcon>

#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QComboBox>
#include <QDateEdit>
#include <QCheckBox>
#include <QHash>

#include <QTableWidget>
#include <QHeaderView>

#include "widgets/group_box_collapsible.h"
#include "network/hydraulic_data.h"
#include <aowis/model/hydraulic/network_hydraulic.h>

#include "common/_enums_structs.h"
#include "common/_sizes.h"

class InterfaceServerMap;
class QMessageBox;
class QGridLayout;

class EntityInspectorWidget : public QWidget
{
    Q_OBJECT
    
public:
    explicit EntityInspectorWidget(HydraulicData *hydraulic_data, QWidget *parent = nullptr);
    void setCurrentTabIndex(int index);

signals:
    void signalCurrentTabChanged(int index);
    
protected:
    HydraulicData *hydraulicData() const;

    //QVBoxLayout *mainLayout() const;
    QVBoxLayout *layoutOverview();
    QVBoxLayout *layoutConfiguration();
    QVBoxLayout *layoutSimMeas();
    QVBoxLayout *layoutQuality();
    QVBoxLayout *layoutQualitySimMeas();
    
    void setTitle(const QString &title);
    
    // Bounds a combo box's preferred width so a single long or dynamically-populated
    // item (e.g. a curve/pattern name, or a "[Missing ...] <uuid>" fallback) cannot
    // force the entity inspector wider than Sizes::SidebarRightWidth. The full text
    // remains visible in the drop-down popup, which is not width-constrained.
    void constrainComboWidth(QComboBox *combo, int minimum_contents_length = 14);
    
    // Bounds a spin box's preferred width. QAbstractSpinBox derives its (non-shrinkable)
    // sizeHint from the formatted min/max text, independent of the value actually shown,
    // so a wide range + many decimals + a multi-character suffix (e.g. "mg/min") can force
    // the entity inspector wider than Sizes::SidebarRightWidth even though the displayed
    // value ("0.000000 mg/L") is short. Typed values remain fully editable; only the
    // widget's width is capped.
    void constrainSpinWidth(QAbstractSpinBox *spin, int maximum_width = 180);
    
    void addGroupOverviewImage(const QString &icon_path, const QString &name);
    QPushButton *button_overview_find_on_map = nullptr;
    
    void addGroupGeneral(const QString &name);
    QLineEdit *line_name = nullptr;
    QDateEdit *date_added = nullptr;
    QDateEdit *date_installed = nullptr;
    QComboBox *combo_model_role = nullptr;
    QCheckBox *check_enabled = nullptr;
    
    void addGroupEndpoints();
    QLabel *label_node_1_id = nullptr;
    QLabel *label_node_2_id = nullptr;
    QPushButton *button_node_1_locate = nullptr;
    QPushButton *button_node_1_inspect = nullptr;
    QPushButton *button_node_2_locate = nullptr;
    QPushButton *button_node_2_inspect = nullptr;
    
    void addGroupPosition();
    void refreshGeographicAltitude();
    QDoubleSpinBox *spin_latitude = nullptr;
    QDoubleSpinBox *spin_longitude = nullptr;
    QLabel *label_geographic_altitude = nullptr;
    QPushButton *button_find_on_map = nullptr;

    void bindHydraulicNode(InfrastructureEntity entity_type, const QUuid &uuid,
                           const QString &title_prefix);
    void bindHydraulicLink(InfrastructureEntity entity_type, const QUuid &uuid,
                           const QString &title_prefix);
    void bindDemandPoint(const QUuid &uuid, const QString &title_prefix);
    
    void addGroupElevation();
    GroupBoxCollapsible *group_elevation = nullptr;
    QComboBox *combo_elevation_input_type = nullptr;
    
    QLabel *label_terrain_elevation = nullptr;
    QPushButton *button_terrain_elevation = nullptr;
    QLabel *label_terrain_elevation_status = nullptr;
    QDoubleSpinBox *spin_terrain_elevation = nullptr;
    QLabel *label_elevation_offset = nullptr;
    QDoubleSpinBox *spin_elevation_offset = nullptr;
    QLabel *label_elevation_value = nullptr;
    QDoubleSpinBox *spin_elevation_value = nullptr;
    QComboBox *combo_head_pattern = nullptr;
    
    void addGroupDemands(bool include_emitter = true);
    QLabel *label_demands_summary = nullptr;
    QDoubleSpinBox *spin_emitter_coefficient = nullptr;
    QDoubleSpinBox *spin_emitter_pressure_exponent = nullptr;
    QLabel *label_emitter_coefficient_unit = nullptr;

    void addGroupSimulation();
    void addGroupWaterQualitySimulation();
    void addGroupNodeQualityInputs();
    void addGroupNoEntitySpecificQualityInputs(const QString &entity_name);

    void openDemandsEditor();
    QPointer<QDialog> dialog_demands = nullptr;
    QPointer<QTableWidget> table_demands = nullptr;
    
    void addGroupAlerts();
    void addGroupHistory();
    
    void addStretches();
    
private:
    enum class SimulationField
    {
        ResultTime,
        DemandRequested,
        DemandDelivered,
        DemandDeficit,
        TotalDemand,
        NetDemand,
        EmitterFlow,
        LeakageFlow,
        Head,
        PressureHead,
        WaterLevel,
        Volume,
        MixingZoneVolume,
        Flow,
        Velocity,
        HeadLoss,
        UnitHeadLoss,
        FrictionFactor,
        Roughness,
        Status,
        PumpState,
        Speed,
        Efficiency,
        Power,
        ValveRegulating,
        Setting,
        ReferencedByControl,
        TimeOnline,
        AverageEfficiency,
        AverageEnergyIntensity,
        AveragePower,
        PeakPower,
        AverageCostPerDay
    };

    struct SimulationRowWidgets
    {
        QLabel *name = nullptr;
        QLabel *value = nullptr;
    };

    void addSimulationRow(QGridLayout *grid, int &row, SimulationField field,
                          const char *name, const char *tooltip = nullptr);
    void addGroupSimulationErrors();
    void refreshSimulationErrors();
    void addGroupSimulationWarnings();
    void refreshSimulationWarnings();
    void refreshSimulation();
    void refreshWaterQualitySimulation();
    void resetSimulationValues();
    void setSimulationText(SimulationField field, const QString &text);
    void setSimulationValue(SimulationField field, double value, int decimals,
                            const QString &unit = QString());
    void setSimulationEntityAvailable(bool available);
    void refreshPumpEnergySummary(const HydraulicSimulationResultTimeline &timeline);
    void refreshNodeQualityInputs();
    void refreshSourceTraceInputs();
    void populateQualitySourcePatternCombo(const QUuid &pattern_uuid);
    void updateQualitySourceInputUi();
    QString sourceTraceNodeDisplayName(const QUuid &uuid) const;
    void requestUseCurrentNodeAsSourceTraceOrigin();

    void refreshHydraulicGeneral(const QString &id, const HydraulicEntityMetadata &metadata);
    void refreshHydraulicNode();
    void refreshHydraulicLink();
    void refreshDemandPoint();
    void refreshHydraulicEndpoints();
    void refreshHydraulicEndpoint(const QUuid &node_uuid, QLabel *label_node_id,
                                  QPushButton *button_locate, QPushButton *button_inspect);
    void locateHydraulicEndpoint(const QUuid &node_uuid);
    void inspectHydraulicEndpoint(const QUuid &node_uuid);
    void refreshHydraulicNodeElevation();
    void scheduleDemandsRefresh();
    void refreshDemands(bool presentation_only = false);
    void refreshDemandSummary();
    void rebuildDemandRows(const QList<HydraulicDemand> &demands);
    void addDemandRow(int demand_index, const HydraulicDemand &demand);
    void updateDemandRow(int demand_index, const HydraulicDemand &demand);
    std::optional<QList<HydraulicDemand>> currentDemands() const;
    bool addCurrentDemand(const HydraulicDemand &demand);
    bool removeCurrentDemand(int demand_index);
    bool setCurrentDemandCategoryName(int demand_index, const QString &category_name);
    bool setCurrentDemandBaseDemandM3PerH(int demand_index, double base_demand_m3_per_h);
    bool setCurrentDemandPatternMode(int demand_index, HydraulicTimePatternMode pattern_mode);
    bool setCurrentDemandPatternUuid(int demand_index, const QUuid &pattern_uuid);
    bool setCurrentDemandSourceMethod(int demand_index, HydraulicDemandSourceMethod source_method);
    bool setCurrentDemandNote(int demand_index, const QString &note);
    void populateTimePatternCombo(QComboBox *combo_pattern, HydraulicTimePatternMode pattern_mode, const QUuid &pattern_uuid);
    void updateElevationModeUi();
    void updateCalculatedElevation();
    void requestTerrainElevation();
    void handleTerrainElevationResponse(const QByteArray &data);
    void handleTerrainElevationError(const QString &error);
    void showTerrainElevationErrorMessage(const QString &error);
    void setTerrainElevationRequestActive(bool active);
    bool setElevationInputType(HydraulicNodeElevationInputType input_type);
    bool setElevationValue(double value_m);
    bool setTerrainElevation(double terrain_elevation_m);
    bool setElevationOffset(double offset_m);
    std::optional<QDate> optionalDate(const QDateEdit *date_edit) const;
    void setOptionalDate(QDateEdit *date_edit, const std::optional<QDate> &date);

    HydraulicData *hydraulic_data = nullptr;
    InfrastructureEntity entity_type = InfrastructureEntity::Unknown;
    QUuid entity_uuid;
    QUuid node_uuid_1;
    QUuid node_uuid_2;
    void refreshEntityTitle();
    QString displayed_entity_id;
    QString entity_title_prefix;
    bool demands_refresh_pending = false;
    bool demands_structure_stale = false;
    InterfaceServerMap *terrain_elevation_client = nullptr;
    QPointer<QMessageBox> terrain_elevation_message_box = nullptr;
    QPointer<QMessageBox> source_trace_origin_message_box = nullptr;
    QUuid terrain_elevation_request_entity_uuid;
    CoordinateWGS84 terrain_elevation_request_coordinate;
    bool terrain_elevation_request_active = false;

    QHash<int, SimulationRowWidgets> simulation_rows;
    QDoubleSpinBox *spin_quality_initial_chemical = nullptr;
    QDoubleSpinBox *spin_quality_initial_water_age = nullptr;
    QComboBox *combo_quality_source_type = nullptr;
    QDoubleSpinBox *spin_quality_source_concentration = nullptr;
    QDoubleSpinBox *spin_quality_source_mass_flow = nullptr;
    QComboBox *combo_quality_source_pattern = nullptr;
    QLabel *label_source_trace_origin_value = nullptr;
    QPushButton *button_source_trace_origin_inspect = nullptr;
    QCheckBox *check_source_trace_origin_current_node = nullptr;
    GroupBoxCollapsible *group_simulation_errors = nullptr;
    QLabel *label_simulation_errors = nullptr;
    GroupBoxCollapsible *group_simulation_warnings = nullptr;
    QLabel *label_simulation_warnings = nullptr;
    QLabel *label_simulation_message = nullptr;
    QLabel *label_simulation_energy_message = nullptr;
    QLabel *label_quality_simulation_message = nullptr;
    QLabel *label_quality_simulation_time = nullptr;
    QLabel *label_quality_simulation_value_name = nullptr;
    QLabel *label_quality_simulation_value = nullptr;
    QLabel *label_quality_simulation_source_mass_name = nullptr;
    QLabel *label_quality_simulation_source_mass = nullptr;
    
    QTabWidget *tabs = nullptr;
    QVBoxLayout *layout_main = nullptr;
    QLabel *label_title = nullptr;
    
    QScrollArea *scroll_overview = nullptr;
    QWidget *widget_overview = nullptr;
    QVBoxLayout *layout_overview = nullptr;
    
    QScrollArea *scroll_configuration = nullptr;
    QWidget *widget_configuration = nullptr;
    QVBoxLayout *layout_configuration = nullptr;
    
    QScrollArea *scroll_sim_meas = nullptr;
    QWidget *widget_sim_meas = nullptr;
    QVBoxLayout *layout_sim_meas = nullptr;
    
    QScrollArea *scroll_quality = nullptr;
    QWidget *widget_quality = nullptr;
    QVBoxLayout *layout_quality = nullptr;

    QScrollArea *scroll_quality_sim_meas = nullptr;
    QWidget *widget_quality_sim_meas = nullptr;
    QVBoxLayout *layout_quality_sim_meas = nullptr;
    
private slots:
    void onGroupExpand(GroupBoxCollapsible *group);
    
    void onElevationInputTypeChanged(int index);
    
public slots:
    virtual void onHeadlossFormulaChanged(HeadlossFormulas formulas);
};

#endif
