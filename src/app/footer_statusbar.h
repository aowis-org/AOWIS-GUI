#ifndef FOOTER_STATUSBAR_H
#define FOOTER_STATUSBAR_H

#include <QObject>
#include <QWidget>
#include <QEvent>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QTimer>
#include <optional>

#include "common/_enums_structs.h"
#include <aowis/model/gis.h>

class FooterStatusBar : public QWidget
{
    Q_OBJECT

public:
    FooterStatusBar(QWidget *parent = nullptr);
    
    // Updates the displayed zoom level. Ignored while the person has the
    // control focused/mid-edit, so a live camera update doesn't yank a
    // value out from under them while they're typing.
    void setMapZoom(double zoom);
    void setMapCoordinatesWGS84(const CoordinateWGS84 &wgs);
    void setMapCoordinatesUTM(const CoordinateUTM &utm);
    void clearMapCoordinates();
    
    QStatusBar* statusBar() const { return this->bar; };
    
    void statusUpdateServerMap(StatusColorCode code);

signals:
    // Fired when the person commits a new value (Enter, focus loss, or the
    // spin buttons/scroll) -- not on every keystroke while typing, see
    // setKeyboardTracking(false) in the constructor.
    void zoomEdited(double zoom);

protected:
    void changeEvent(QEvent *event) override;

private:
    void refreshLocalizedText();
    void refreshCoordinateText();
    // Height is a datum-dependent observation; keep it missing until supplied.
    void refreshAltitudeDisplay();
    // The map setters above arrive every frame while the camera moves (from
    // both maps when their movement is synced). Each label text change
    // relayouts and repaints the status bar, which in the browser costs a
    // backing-store flush per frame: the latest values are kept and shown at
    // most every DisplayIntervalMs.
    void scheduleDisplay();
    void applyPendingDisplay();
    static constexpr int DisplayIntervalMs = 100;
    QTimer *display_timer = nullptr;
    std::optional<double> pending_zoom;
    std::optional<CoordinateWGS84> pending_wgs84;
    std::optional<CoordinateUTM> pending_utm;

    QStatusBar *bar;
    
    QHBoxLayout *layout;
    
    QDoubleSpinBox *spin_map_zoom;
    QLabel *label_map_coords_lat;
    QLabel *label_map_coords_lon;
    QLabel *label_map_coords_height;
    std::optional<double> coordinate_altitude_m;
    std::optional<CoordinateWGS84> current_wgs84;
    std::optional<CoordinateUTM> current_utm;
    
    QLabel *label_map_coords_utm_easting;
    QLabel *label_map_coords_utm_northing;
    
    QLabel *label_indicator_map;
    QLabel *label_indicator_map_status;
    QLabel *label_indicator_server;
};

#endif // FOOTER_STATUSBAR_H
