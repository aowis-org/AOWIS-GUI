#include "app/footer_statusbar.h"

#include <QEvent>
#include <QSignalBlocker>
#include "config/geographic_altitude_presentation.h"
#include "config/unit_profile_manager.h"

FooterStatusBar::FooterStatusBar(QWidget *parent)
    : QWidget(parent),
    spin_map_zoom( new QDoubleSpinBox ),
    label_map_coords_lat( new QLabel ),
    label_map_coords_lon( new QLabel ),
    label_map_coords_height( new QLabel ),
    label_map_coords_utm_easting( new QLabel ),
    label_map_coords_utm_northing( new QLabel ),
    label_indicator_map( new QLabel ),
    label_indicator_map_status(new QLabel ),
    label_indicator_server( new QLabel )
{
    this->bar = new QStatusBar(this);
    
    this->layout = new QHBoxLayout(this);
    this->layout->addWidget(this->bar);
    
    // Matches MapModel::MinZoom/MaxZoom (1-19); kept as a literal here
    // rather than depending on map/core/ from app/.
    this->spin_map_zoom->setRange(1.0, 19.0);
    this->spin_map_zoom->setDecimals(2);
    this->spin_map_zoom->setSingleStep(0.1);
    this->spin_map_zoom->setPrefix(tr("Zoom: "));
    this->spin_map_zoom->setToolTip(tr(
        "Current map zoom level. Type a value or use the arrows/scroll to change it."));
    // Only commit on Enter/focus-loss/step-buttons, not on every keystroke
    // -- otherwise typing "15" would briefly jump to zoom 1 first.
    this->spin_map_zoom->setKeyboardTracking(false);
    this->spin_map_zoom->setMinimumWidth(110);
    this->label_map_coords_lat->setMinimumWidth(135);
    this->label_map_coords_lon->setMinimumWidth(112);
    this->label_map_coords_height->setMinimumWidth(115);
    this->label_map_coords_height->setToolTip(tr(
        "Terrain elevation at the map pointer, when a DEM sample is available. "
        "Uses the terrain dataset's vertical datum; unavailable data is shown as —."));
    
    this->label_map_coords_utm_easting->setMinimumWidth(155);
    this->label_map_coords_utm_northing->setMinimumWidth(100);
    
    this->bar->addWidget(this->spin_map_zoom);
    this->bar->addWidget(this->label_map_coords_lat);
    this->bar->addWidget(this->label_map_coords_lon);
    this->bar->addWidget(this->label_map_coords_height);
    
    this->bar->addWidget(this->label_map_coords_utm_easting);
    this->bar->addWidget(this->label_map_coords_utm_northing);
    
    this->bar->addPermanentWidget(this->label_indicator_map);
    
    #ifdef AOWIS_STANDALONE
    this->label_indicator_map->setText(tr("[AOWIS Controller Standalone Build]"));
    #else
    this->label_indicator_map->setText(tr("AOWIS Map Server"));
    this->label_indicator_server->setText(tr("AOWIS Server"));
    this->bar->addPermanentWidget(this->label_indicator_server);
    #endif
    
    //this->label_indicator_map_status->setFixedSize(12, 12);
    //this->label_indicator_map_status->setStyleSheet("background-color: green; border-radius: 6px;");

    UnitProfileManager::instance().presentation().subscribe(this, [this](UnitPresentationContext::Generation) {
        this->refreshAltitudeDisplay();
    });
    this->refreshAltitudeDisplay();

    connect(this->spin_map_zoom, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
        [this](double value)
    {
        emit zoomEdited(value);
    });

    connect(this->spin_map_zoom, &QDoubleSpinBox::editingFinished, this, [this]
    {
        // Process the edit before altering its visual prefix.
        QTimer::singleShot(0, this, [this] { this->refreshLocalizedText(); });
    });

    this->display_timer = new QTimer(this);
    this->display_timer->setSingleShot(true);
    this->display_timer->setInterval(DisplayIntervalMs);
    connect(this->display_timer, &QTimer::timeout, this, &FooterStatusBar::applyPendingDisplay);
}

void FooterStatusBar::scheduleDisplay()
{
    // Leading edge after a quiet period (an isolated change shows at once),
    // then at most one update per interval while values keep coming.
    if (this->display_timer->isActive())
        return;
    applyPendingDisplay();
    this->display_timer->start();
}

void FooterStatusBar::applyPendingDisplay()
{
    if (this->pending_zoom.has_value())
    {
        const double zoom = this->pending_zoom.value();
        this->pending_zoom.reset();
        // Not while the person edits the value (see setMapZoom()).
        if (!this->spin_map_zoom->hasFocus())
        {
            const QSignalBlocker blocker(this->spin_map_zoom);
            this->spin_map_zoom->setValue(zoom);
        }
    }
    if (this->pending_wgs84.has_value())
    {
        const CoordinateWGS84 wgs = this->pending_wgs84.value();
        this->pending_wgs84.reset();
        this->current_wgs84 = wgs;
        this->coordinate_altitude_m = wgs.altitude_m;
        this->refreshAltitudeDisplay();
        this->refreshCoordinateText();
    }
    if (this->pending_utm.has_value())
    {
        const CoordinateUTM utm = this->pending_utm.value();
        this->pending_utm.reset();
        this->current_utm = utm;
        this->refreshCoordinateText();
    }
}

void FooterStatusBar::setMapZoom(double zoom)
{
    if (this->spin_map_zoom->hasFocus())
        return;

    this->pending_zoom = zoom;
    scheduleDisplay();
}
void FooterStatusBar::setMapCoordinatesWGS84(const CoordinateWGS84 &wgs)
{
    this->pending_wgs84 = wgs;
    scheduleDisplay();
}
void FooterStatusBar::setMapCoordinatesUTM(const CoordinateUTM &utm)
{
    this->pending_utm = utm;
    scheduleDisplay();
}

void FooterStatusBar::refreshAltitudeDisplay()
{
    NumericFormatPolicy footer_height_policy;
    footer_height_policy.notation = NumericFormatPolicy::Notation::Fixed;
    footer_height_policy.precision = 2;
    this->label_map_coords_height->setText(tr("Height: ")
        + GeographicAltitudePresentation::format(
            UnitProfileManager::instance().presentation(), this->coordinate_altitude_m,
            footer_height_policy));
}

void FooterStatusBar::refreshCoordinateText()
{
    // Coordinates are observations, not translated user data. Keep the last
    // observed values so an idle map updates immediately after LanguageChange.
    if (this->current_wgs84.has_value())
    {
        const CoordinateWGS84 &wgs = *this->current_wgs84;
        this->label_map_coords_lat->setText(tr("WGS 84 Lat: %1").arg(
            QString::number(wgs.latitude_deg, 'f', 5)));
        this->label_map_coords_lon->setText(tr("Lon: %1").arg(
            QString::number(wgs.longitude_deg, 'f', 5)));
    }
    if (this->current_utm.has_value())
    {
        const CoordinateUTM &utm = *this->current_utm;
        const QString hemisphere = utm.hemisphere_northern ? QStringLiteral("N") : QStringLiteral("S");
        const QString crs = utm.isUPS()
            ? QStringLiteral("UPS ") + hemisphere
            : QStringLiteral("UTM ") + QString::number(utm.zone) + hemisphere;
        this->label_map_coords_utm_easting->setText(tr("%1 E: %2 m").arg(
            crs, QString::number(utm.easting_m, 'f', 2)));
        this->label_map_coords_utm_northing->setText(tr("N: %1 m").arg(
            QString::number(utm.northing_m, 'f', 2)));
    }
}

void FooterStatusBar::refreshLocalizedText()
{
    // QDoubleSpinBox::setPrefix can reset an uncommitted edit. Do not touch
    // that editor while it has focus; editingFinished applies the new prefix.
    if (!this->spin_map_zoom->hasFocus())
    {
        const QSignalBlocker blocker(this->spin_map_zoom);
        this->spin_map_zoom->setPrefix(tr("Zoom: "));
    }
    this->spin_map_zoom->setToolTip(tr(
        "Current map zoom level. Type a value or use the arrows/scroll to change it."));
    this->label_map_coords_height->setToolTip(tr(
        "Terrain elevation at the map pointer, when a DEM sample is available. "
        "Uses the terrain dataset's vertical datum; unavailable data is shown as —."));
#ifdef AOWIS_STANDALONE
    this->label_indicator_map->setText(tr("[AOWIS Controller Standalone Build]"));
#else
    this->label_indicator_map->setText(tr("AOWIS Map Server"));
    this->label_indicator_server->setText(tr("AOWIS Server"));
#endif
    this->refreshAltitudeDisplay();
    this->refreshCoordinateText();
}

void FooterStatusBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        this->refreshLocalizedText();
}

void FooterStatusBar::clearMapCoordinates()
{
    // A throttled update must not bring the values back.
    this->pending_wgs84.reset();
    this->pending_utm.reset();
    this->coordinate_altitude_m.reset();
    this->current_wgs84.reset();
    this->current_utm.reset();
    this->refreshAltitudeDisplay();
    this->label_map_coords_lat->clear();
    this->label_map_coords_lon->clear();
    this->label_map_coords_utm_easting->clear();
    this->label_map_coords_utm_northing->clear();
}

void FooterStatusBar::statusUpdateServerMap(StatusColorCode code)
{
    QString style = "color: black;";
    switch (code)
    {
        case StatusColorCode::None:   style = "color: black";   break;
        case StatusColorCode::Green:  style = "color: green;";  break;
        case StatusColorCode::Yellow: style = "color: yellow;"; break;
        case StatusColorCode::Red:    style = "color: red;";    break;
    }
    
    this->label_indicator_map->setStyleSheet(style);
}

