#ifndef MAP_NAVIGATION_WIDGET_H
#define MAP_NAVIGATION_WIDGET_H

#include <QObject>
#include <QEvent>
#include <QWidget>
#include <QGridLayout>

#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <QIcon>
#include <QAbstractButton>

#include <QTimer>

class QKeyEvent;
class QKeySequence;
class QShortcut;

#include "map/core/map_widget.h"
#include "common/_enums_structs.h"
#include "network/network_symbology.h"
#include "map/render/map_label_settings.h"

class MapNavigationWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MapNavigationWidget(MapWidget *map, CanvasMode mode, QWidget *keyboard_focus_target, QWidget *parent = nullptr);
    
    void mapProviderChange(MapProvider provider);
    // The map label switches (all off initially).
    MapLabelSettings labelSettings() const;
    // The network this map shows has entities (true) or none (false): the
    // opacity slider starts leftmost (imagery unfaded) without a network,
    // moves to NetworkOpacityPercent when a network appears and back to the
    // left when it is gone. Changes in between keep the user's position.
    void setNetworkPresent(bool present);
    static constexpr int NetworkOpacityPercent = 50;
    // When fullscreen hides the sidebar, Qt stops dispatching its keyboard
    // shortcuts. The map forwards key presses here to trigger the provider
    // shortcuts or a matching visible-on-page button.
    static bool triggerButtonShortcut(QWidget *root, const QKeyEvent *event);
    
protected:
    void changeEvent(QEvent *event) override;

private:
    CanvasMode mode;
    QGridLayout *grid;
    MapWidget *map;
    QWidget *keyboard_focus_target;
    
    QLabel *label_map_opacity = nullptr;
    QLabel *label_labels_title = nullptr;
    QComboBox *combo_provider = nullptr;
    QShortcut *provider_shortcuts[4] = {nullptr, nullptr, nullptr, nullptr};
    void refreshProviderChoices();
    void refreshLocalizedText();
    bool triggerProviderShortcut(const QKeySequence &pressed);
    
    QCheckBox *check_3d_icons = nullptr;
    // Map labels: IDs (monitor and editor), values of the colouring metric
    // (monitor only).
    QCheckBox *check_label_node_ids = nullptr;
    QCheckBox *check_label_link_ids = nullptr;
    QCheckBox *check_label_node_values = nullptr;
    QCheckBox *check_label_link_values = nullptr;
    QSlider *slider_icon_size = nullptr;
    QLabel *label_icon_size = nullptr;
    QSlider *slider_map_opacity = nullptr;
    bool network_present = false;
    QComboBox *combo_icon_size_unit = nullptr;
    int icon_size_2d_percent = 100;
    int icon_size_3d_percent = 100;
    NetworkSymbologySizeUnit icon_size_2d_unit = NetworkSymbologySizeUnit::Pixels;
    int icon_size_2d_px = NetworkSymbologyDefaultIconSizePx;
    double icon_size_2d_m = NetworkSymbologyDefaultIconSizeM;

    void activateMapProvider(MapProvider provider);
    void sync3dIconsForViewMode(MapViewMode view_mode);
    void syncIconSizeSliderForViewMode(MapViewMode view_mode);
    void refreshIconSizeUnitPresentation();
    void refreshShortcutPresentation();
    QWidget *createLabelControls();
    
signals:
    void signalSlideOpacityChanged(int opacity);
    void signalIconSizeChanged(int size_percent);
    void signalMonitorIconSizeUnitChanged(NetworkSymbologySizeUnit unit);
    void signalMonitorIconSizeChanged(NetworkSymbologySizeUnit unit, double size);
    void signal3dIconsChanged(bool enabled);
    void signalLabelSettingsChanged(const MapLabelSettings &settings);
    
};

#endif // MAP_NAVIGATION_WIDGET_H
