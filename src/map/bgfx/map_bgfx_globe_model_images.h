#ifndef MAP_BGFX_GLOBE_MODEL_IMAGES_H
#define MAP_BGFX_GLOBE_MODEL_IMAGES_H

#include <QImage>

// The desktop host owns the existing Qt application resources. The shared
// renderer owns 3D mesh geometry, not these GUI-provided texture assets.
QImage mapGlobeTankAlbedoImage();
QImage mapGlobeReservoirAlbedoImage();

#endif // MAP_BGFX_GLOBE_MODEL_IMAGES_H
