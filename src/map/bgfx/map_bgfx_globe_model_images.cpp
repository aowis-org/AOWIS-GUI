#include "map/bgfx/map_bgfx_globe_model_images.h"

#include <QString>

QImage mapGlobeTankAlbedoImage()
{
    QImage image(QStringLiteral(":/map-3d/tank_albedo.png"));
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

QImage mapGlobeReservoirAlbedoImage()
{
    QImage image(QStringLiteral(":/map-3d/reservoir_albedo.png"));
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
