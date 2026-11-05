#include "map/bgfx/bgfx_device_manager.h"
#include "map/bgfx/map_bgfx_backend.h"

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
