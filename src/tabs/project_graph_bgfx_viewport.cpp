#include "tabs/project_graph_bgfx_viewport.h"

#include "config/gui_configuration.h"
#include "map/bgfx/bgfx_antialiasing.h"
#include "map/bgfx/bgfx_device_manager.h"
#include "map/bgfx/bgfx_qt_native_surface.h"
#include "map/bgfx/map_bgfx_backend.h"
#include "tabs/project_graph_bgfx_scene.h"
#include "tabs/project_revision_graph.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QSurface>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <QVector>
#include <QDebug>

#include <algorithm>
#include <limits>

namespace
{
std::uint16_t targetDimension(int value)
{
    return std::uint16_t(std::clamp(value, 1, int(std::numeric_limits<std::uint16_t>::max())));
}

constexpr std::uint32_t kGraphResetFlags =
    BGFX_RESET_VSYNC | BGFX_RESET_FLIP_AFTER_RENDER;

// The map's antialiasing setting also applies to the graph: both own the
// same shared bgfx device in turn. MSAA is per surface in the pinned bgfx
// (SwapChain::flags; a BGFX_RESET_MSAA_* reset flag is ignored).
std::uint32_t graphSwapChainFlags()
{
    return bgfxMsaaSwapChainFlag(guiConfiguration().map_performance.antialiasing_samples);
}
}

ProjectGraphBgfxViewport::ProjectGraphBgfxViewport(ProjectRevisionGraph *graph)
    : QObject(graph), graph(graph), atlas(ShapeAtlasExtent)
{
    this->retry_timer.setInterval(350);
    connect(&this->retry_timer, &QTimer::timeout, this,
            [this]() { checkAvailability(); });
    this->redraw_timer.setSingleShot(true);
    connect(&this->redraw_timer, &QTimer::timeout, this,
            [this]() { render(); });
}

ProjectGraphBgfxViewport::~ProjectGraphBgfxViewport()
{
    this->retry_timer.stop();
    this->redraw_timer.stop();
    shutdownOwnedDevice();
    detach();
    if (this->window != nullptr)
        this->window->removeEventFilter(this);
    if (this->container != nullptr)
        this->container->removeEventFilter(this);
    // Qt's createWindowContainer() owns the QWindow once it is embedded.
}

void ProjectGraphBgfxViewport::onShown()
{
    if (!this->retry_timer.isActive())
        this->retry_timer.start();
    checkAvailability();
}

void ProjectGraphBgfxViewport::onHidden()
{
    this->retry_timer.stop();
    this->redraw_timer.stop();
    shutdownOwnedDevice();
    detach();
    if (this->container != nullptr)
        this->container->hide();
}

void ProjectGraphBgfxViewport::resize()
{
    if (this->container != nullptr)
        this->container->setGeometry(this->graph->rect());
    requestRedraw();
}

void ProjectGraphBgfxViewport::requestRedraw()
{
    if (this->attached && this->graph->isVisible() && !this->redraw_timer.isActive())
        this->redraw_timer.start(0);
}

void ProjectGraphBgfxViewport::invalidateScene()
{
    // Graph rebuilds (including fold/unfold or orientation changes) are the
    // only source of scene-index invalidation. Camera motion reuses it.
    this->scene_index.clear();
    this->scene_dirty = true;
    requestRedraw();
}

void ProjectGraphBgfxViewport::checkAvailability()
{
    if (this->unavailable || !this->graph->isVisible())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    // The Projects graph may now be the first bgfx client. Do not check swap-
    // chain capabilities until a device exists: the first window becomes its
    // default framebuffer and does not require secondary-swapchain support.
    const bgfx::Caps *caps = device.hasOwner() ? bgfx::getCaps() : nullptr;
    if (device.hasOwner() && !device.owns(this)
        && (caps == nullptr || (caps->supported & BGFX_CAPS_SWAP_CHAIN) == 0))
    {
        this->unavailable = true;
        this->retry_timer.stop();
        return;
    }
    if (this->attached)
    {
        if (!device.isCurrent(this, this->device_generation)
            || !device.frames().views(this).valid() || !device.targets().contains(this))
        {
            // A stale generation must never destroy handles on the new one.
            detach();
        }
        else
            return;
    }
    if (this->window == nullptr)
    {
        this->window = new QWindow();
        this->window->setSurfaceType(QSurface::OpenGLSurface);
        this->window->setTitle(QStringLiteral("AOWIS Projects bgfx viewport"));
        this->window->installEventFilter(this);
        this->container = QWidget::createWindowContainer(this->window, this->graph);
        this->container->setObjectName(QStringLiteral("projectGraphBgfxContainer"));
        this->container->setFocusPolicy(Qt::NoFocus);
        // Qt may send the first click to the container while activating its
        // embedded native QWindow. Forward that press too, so the graph can
        // select on the very first interaction instead of requiring another.
        this->container->installEventFilter(this);
        this->container->setGeometry(this->graph->rect());
    }
    this->container->show();
    QTimer::singleShot(0, this, [this]() { tryAttach(); });
}

QSize ProjectGraphBgfxViewport::targetPixels() const
{
    if (this->window == nullptr)
        return QSize(1, 1);
    const qreal ratio = this->window->devicePixelRatio();
    return QSize(std::max(1, qRound(this->window->width() * ratio)),
                 std::max(1, qRound(this->window->height() * ratio)));
}

bool ProjectGraphBgfxViewport::initializePrimary()
{
    if (this->window == nullptr || !this->window->isExposed())
        return false;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (device.hasOwner() || !device.prepareFor(this))
        return false;

    bgfx::Init settings{};
    const BgfxNativeSurface surface = BgfxQtNativeSurface::capture(this->window);
    if (!surface.valid())
        return false;
    surface.configurePrimary(&settings);
    settings.reset = kGraphResetFlags;
    settings.swapChain.flags = graphSwapChainFlags();
    this->allocated_swap_chain_flags = settings.swapChain.flags;
    settings.limits.maxTransientVbSize = 16u << 20;
    settings.limits.maxTransientIbSize = 8u << 20;

    QVector<bgfx::RendererType::Enum> attempts;
    const bgfx::RendererType::Enum requested = MapBgfxBackend::requestedRenderer();
    if (requested != bgfx::RendererType::Count)
        attempts.append(requested);
    attempts.append(bgfx::RendererType::Count);
    const QVector<bgfx::RendererType::Enum> usable = MapBgfxBackend::usableRenderers();
    for (const bgfx::RendererType::Enum renderer : usable)
    {
        if (!attempts.contains(renderer))
            attempts.append(renderer);
    }

    for (const bgfx::RendererType::Enum attempt : attempts)
    {
        settings.type = attempt;
        if (!device.initialize(this, settings,
                               [this]() { return this->graph->isVisible(); },
                               [this]() { shutdownOwnedDevice(); }))
            continue;
        const bgfx::RendererType::Enum selected = bgfx::getRendererType();
        if (MapBgfxBackend::shadersAvailable(selected))
        {
            qInfo() << "Projects bgfx viewport: initialized primary device with"
                    << bgfx::getRendererName(selected);
            return true;
        }
        qWarning() << "Projects bgfx viewport: no embedded shaders for"
                   << bgfx::getRendererName(selected);
        device.shutdown(this);
    }
    return false;
}

void ProjectGraphBgfxViewport::shutdownOwnedDevice()
{
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (!device.owns(this))
        return;
    // Secondary GPU clients must relinquish their resources before the
    // primary framebuffer or graph shaders are destroyed.
    if (!device.retireClients(this))
    {
        qWarning() << "Projects bgfx viewport: secondary client refused retirement";
        return;
    }
    detach();
    if (!device.shutdown(this))
        qWarning() << "Projects bgfx viewport: device shutdown refused";
}

void ProjectGraphBgfxViewport::retireFromDevice()
{
    // The primary owner is relinquishing its bgfx generation. Release our
    // shaders and swapchain synchronously while it is still alive; retry the
    // graph on the next event-loop turn after the old device is gone.
    detach();
    if (this->container != nullptr)
        this->container->hide();
    if (this->graph->isVisible())
    {
        if (!this->retry_timer.isActive())
            this->retry_timer.start();
        this->graph->update();
        QTimer::singleShot(0, this, [this]() { checkAvailability(); });
    }
}

void ProjectGraphBgfxViewport::tryAttach()
{
    if (this->attached || this->unavailable || this->window == nullptr
        || !this->window->isExposed() || !this->graph->isVisible())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    // A graph opened before Maps owns the default swapchain. Otherwise it
    // attaches a secondary swapchain to the already-running shared device.
    if (!device.hasOwner() && !initializePrimary())
    {
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: primary initialization failed; using QPainter";
        this->graph->update();
        return;
    }
    const bool primary = device.owns(this);
    const bgfx::Caps *caps = bgfx::getCaps();
    if (!primary && (caps == nullptr || (caps->supported & BGFX_CAPS_SWAP_CHAIN) == 0))
    {
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: secondary swapchain unsupported; using QPainter";
        this->graph->update();
        return;
    }

    const BgfxNativeSurface surface = BgfxQtNativeSurface::capture(this->window);
    if (!surface.valid())
        return;
    this->device_generation = device.generation();
    if (!primary && !device.registerClient(this, this->device_generation,
                                           [this]() { retireFromDevice(); }))
    {
        this->device_generation = 0;
        return;
    }
    const BgfxFrameCoordinator::ViewRange views = device.frames().reserve(this, 1);
    if (!views.valid())
    {
        if (primary)
            shutdownOwnedDevice();
        else
            device.unregisterClient(this, this->device_generation);
        this->device_generation = 0;
        return;
    }
    const QSize pixels = targetPixels();
    const bool target_ready = primary
        ? device.targets().attachDefault(this, views.first)
        : device.targets().attachWindow(this, views.first, surface, true);
    if (!target_ready)
    {
        device.frames().release(this);
        if (primary)
            shutdownOwnedDevice();
        else
            device.unregisterClient(this, this->device_generation);
        this->device_generation = 0;
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: could not attach render target; using QPainter";
        this->graph->update();
        return;
    }
    if (!this->gpu.initialize() || !this->text_gpu.initialize())
    {
        this->gpu.release();
        this->text_gpu.release();
        device.targets().release(this);
        device.frames().release(this);
        if (primary)
            shutdownOwnedDevice();
        else
            device.unregisterClient(this, this->device_generation);
        this->device_generation = 0;
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: GPU HUD or text shader unavailable; using QPainter";
        this->graph->update();
        return;
    }
    this->gpu_initialized = true;
    this->allocated_pixels = pixels;
    this->view_id = views.first;
    this->attached = true;
    this->retry_timer.stop();
    qInfo() << "Projects bgfx viewport: attached shared device view" << this->view_id;
    requestRedraw();
    // bgfx presents the previous submission with its next frame. Ensure
    // static graphs also get a second frame without requiring mouse motion.
    QTimer::singleShot(30, this, [this]() { requestRedraw(); });
}

void ProjectGraphBgfxViewport::detach()
{
    if (!this->attached && !this->gpu_initialized)
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (device.isCurrent(this, this->device_generation))
    {
        if (this->gpu_initialized)
        {
            this->gpu.release();
            this->text_gpu.release();
        }
        device.targets().release(this);
        device.frames().release(this);
        if (!device.owns(this))
            device.unregisterClient(this, this->device_generation);
    }
    else
    {
        // The old device has been destroyed; deleting its GPU handles on a
        // replacement device is invalid. Discard their CPU-side wrappers.
        this->gpu = MapBgfxHudGpu();
        this->atlas = MapBgfxHudTextCache(ShapeAtlasExtent);
        // The glyph atlas is CPU data: kept, uploaded whole to the new texture.
        this->text_gpu = MapBgfxLabelGpu();
    }
    this->gpu_initialized = false;
    this->attached = false;
    this->allocated_pixels = QSize();
    this->view_id = 0;
    this->device_generation = 0;
}

void ProjectGraphBgfxViewport::render()
{
    if (!this->attached || !this->graph->isVisible() || this->window == nullptr
        || !this->window->isExposed())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (!device.isCurrent(this, this->device_generation)
        || !device.frames().views(this).valid())
    {
        detach();
        if (this->container != nullptr)
            this->container->hide();
        this->graph->update();
        return;
    }
    const QSize pixels = targetPixels();
    // Only the device owner's back buffer carries MSAA (a secondary
    // swapchain framebuffer has none): its setting change resets too.
    const std::uint32_t swap_chain_flags = graphSwapChainFlags();
    const bool swap_chain_flags_changed =
        device.owns(this) && swap_chain_flags != this->allocated_swap_chain_flags;
    if (pixels != this->allocated_pixels || swap_chain_flags_changed)
    {
        if (device.owns(this))
        {
            const BgfxNativeSurface surface = BgfxQtNativeSurface::capture(this->window);
            if (!surface.valid())
                return;
            bgfx::SwapChain swap_chain = surface.swapChain();
            swap_chain.flags = swap_chain_flags;
            bgfx::reset(kGraphResetFlags, &swap_chain);
            this->allocated_swap_chain_flags = swap_chain_flags;
        }
        else if (!device.targets().resizeWindow(this,
                         BgfxQtNativeSurface::capture(this->window)))
        {
            qWarning() << "Projects bgfx viewport: secondary swapchain resize failed";
            return;
        }
        this->allocated_pixels = pixels;
    }
    this->atlas.setDevicePixelRatio(this->window->devicePixelRatio());
    if (this->window->screen() != nullptr)
        this->atlas.setLogicalDpi(this->window->screen()->logicalDotsPerInch());

    ProjectGraphBgfxScene::View view;
    view.offset = this->graph->offset;
    view.viewport = QRectF(QPointF(0, 0), QSizeF(this->graph->size()));
    view.zoom = this->graph->zoom;
    view.top_down = this->graph->top_down;
    view.selection_id = this->graph->selected_revision;
    view.palette = this->graph->palette();
    view.font = this->graph->font();
    if (this->scene_dirty)
    {
        QVector<ProjectGraphBgfxScene::Fold> folds;
        folds.reserve(this->graph->omissions.size());
        for (const ProjectRevisionGraph::Omission &omission : this->graph->omissions)
            folds.append({omission.visible_parent_id, omission.visible_child_id,
                          omission.position, omission.revision_count});
        this->scene_index.rebuild(this->graph->nodes, folds, this->graph->top_down);
        this->scene_dirty = false;
    }
    ProjectGraphBgfxScene::TextTarget text_target;
    text_target.canvas = &this->text_canvas;
    text_target.glyphs = &this->glyphs;
    ProjectGraphBgfxScene::Stats stats = ProjectGraphBgfxScene::build(
        &this->canvas, &this->atlas, text_target, this->scene_index, view);
    // The HUD atlas may evict while building: rebuild once with fresh texel
    // references (the glyph atlas never evicts during a build).
    if (this->atlas.evictedSinceFrameStart())
        stats = ProjectGraphBgfxScene::build(&this->canvas, &this->atlas, text_target,
                                             this->scene_index, view);
    bgfx::setViewRect(this->view_id, 0, 0,
                      targetDimension(pixels.width()), targetDimension(pixels.height()));
    bgfx::setViewMode(this->view_id, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(this->view_id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x121820ff);
    bgfx::setViewTransform(this->view_id, nullptr, nullptr);
    bgfx::touch(this->view_id);
    this->gpu.uploadAtlas(&this->atlas);
    this->gpu.draw(this->view_id, this->canvas, QSizeF(this->graph->size()));
    // Text on top of the cards; no halo (the cards are its background).
    this->text_gpu.uploadAtlas(&this->glyphs);
    this->text_gpu.draw(this->view_id, this->text_canvas, QSizeF(this->graph->size()),
                        0x00000000u, 0.0, this->window->devicePixelRatio());
    // The Maps viewport retains its view reservation when its tab is hidden.
    // present() deliberately refuses multiple registered clients, so the
    // graph must submit its already-queued GPU commands through the shared
    // multi-client entry point. A global bgfx frame does not require every
    // reserved view to draw on every tick (hidden maps remain idle).
    if (!device.frames().renderAndPresent({{this, {}}}))
        qWarning() << "Projects bgfx viewport: frame coordinator refused submission";
    // Text still waiting for glyph distance fields (first frames only).
    if (stats.text_deferred > 0)
        requestRedraw();
}

bool ProjectGraphBgfxViewport::eventFilter(QObject *watched, QEvent *event)
{
    const bool from_window = watched == this->window;
    const bool from_container = watched == this->container;
    if (!from_window && !from_container)
        return QObject::eventFilter(watched, event);
    if (from_window && event->type() == QEvent::PlatformSurface)
    {
        QPlatformSurfaceEvent *surface_event = static_cast<QPlatformSurfaceEvent *>(event);
        if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        {
            shutdownOwnedDevice();
            detach();
            return false;
        }
    }
    if (from_window && (event->type() == QEvent::Expose || event->type() == QEvent::Resize))
    {
        QTimer::singleShot(0, this, [this]() { tryAttach(); requestRedraw(); });
        return false;
    }
    switch (event->type())
    {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            // The embedded window and its container fill the graph. Their
            // mouse/wheel positions match graph-local coordinates; the graph
            // alone owns activation, dragging, and keyboard navigation.
            // The graph also suppresses duplicate native/container presses.
            QCoreApplication::sendEvent(this->graph, event);
            return true;
        default:
            return QObject::eventFilter(watched, event);
    }
}
