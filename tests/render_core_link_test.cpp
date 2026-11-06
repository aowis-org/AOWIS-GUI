#include "map/bgfx/bgfx_device_manager.h"
#include "map/bgfx/map_bgfx_backend.h"
#include "map/bgfx/map_bgfx_globe_vertex_layouts.h"
#include "map/bgfx/map_bgfx_globe_terrain_buffer_pool.h"
#include "map/bgfx/map_bgfx_globe_terrain_gpu.h"

#include <QCoreApplication>
#include <QFile>
#include <QResource>

#include <cstdlib>
#include <iostream>

#ifndef AOWIS_RENDER_TEST_SHADER_PATH
#error AOWIS_RENDER_TEST_SHADER_PATH must name an embedded shader family
#endif

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    MapBgfxBackend::initializeEmbeddedShaders();
    const QString resource = QString::fromLatin1(AOWIS_RENDER_TEST_SHADER_PATH);
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0)
    {
        std::cerr << "Renderer static-library shader resource unavailable: "
                  << resource.toStdString() << '\n';
        return EXIT_FAILURE;
    }

    // Link the production globe layout implementation, not just its header.
    // Invalid zero strides are rejected before bgfx initialization, so this
    // catches a missing RenderCore object without creating a GPU device.
    MapBgfxGlobeVertexLayouts globe_layouts;
    if (globe_layouts.initialize(MapBgfxGlobeVertexLayouts::Strides{}))
    {
        std::cerr << "Invalid globe vertex layout unexpectedly accepted\n";
        return EXIT_FAILURE;
    }

    // Resolve the production terrain buffer-pool object, not merely its
    // mock test. release() is safe without a bgfx device when never allocated.
    MapBgfxGlobeTerrainBufferPool terrain_buffers;
    terrain_buffers.release();
    if (bgfx::isValid(terrain_buffers.window()))
    {
        std::cerr << "Fresh terrain buffer pool contains stale handles\n";
        return EXIT_FAILURE;
    }

    // Resolve the production terrain draw/resource implementation from the
    // RenderCore archive. release() is safe without an initialized bgfx GPU.
    MapBgfxGlobeTerrainGpu terrain_gpu;
    terrain_gpu.release();
    if (terrain_gpu.ready())
    {
        std::cerr << "New terrain GPU resource pool unexpectedly ready\n";
        return EXIT_FAILURE;
    }

    // Confirm a non-header-only renderer symbol links without any GUI source.
    BgfxDeviceManager manager;
    if (manager.hasOwner() || manager.generation() != 0 || manager.secondaryClientCount() != 0)
    {
        std::cerr << "Fresh renderer device manager contains stale state\n";
        return EXIT_FAILURE;
    }
    std::cout << "Renderer static library and shader resources: OK ("
              << file.size() << " bytes)\n";
    return EXIT_SUCCESS;
}
