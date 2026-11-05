#include "tabs/project_graph_bgfx_viewport.h"

#include "map/bgfx/bgfx_device_manager.h"
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
#include <QDebug>

#if defined(AOWIS_BGFX_NATIVE_WAYLAND_PRIVATE)
#include <qpa/qplatformnativeinterface.h>
#endif

#include <algorithm>
#include <limits>

namespace
{
std::uint16_t targetDimension(int value)
{
    return std::uint16_t(std::clamp(value, 1, int(std::numeric_limits<std::uint16_t>::max())));
}
}

ProjectGraphBgfxViewport::ProjectGraphBgfxViewport(ProjectRevisionGraph *graph)
    : QObject(graph), graph(graph), atlas(2048)
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
    detach();
    if (this->window != nullptr)
        this->window->removeEventFilter(this);
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

void ProjectGraphBgfxViewport::checkAvailability()
{
    if (this->unavailable || !this->graph->isVisible())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (!device.hasOwner())
        return; // The Projects-first path retains QPainter until device startup is generalized.
    const bgfx::Caps *caps = bgfx::getCaps();
    if (caps == nullptr || (caps->supported & BGFX_CAPS_SWAP_CHAIN) == 0)
    {
        this->unavailable = true;
        this->retry_timer.stop();
        return;
    }
    if (this->attached)
    {
        if (!device.frames().views(this).valid() || !device.targets().contains(this))
        {
            // A device generation ended while the graph was hidden. All old
            // bgfx handles are now invalid and must not be destroyed.
            this->attached = false;
            this->gpu_initialized = false;
            this->gpu = MapBgfxHudGpu();
            this->atlas = MapBgfxHudTextCache(2048);
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

void ProjectGraphBgfxViewport::tryAttach()
{
    if (this->attached || this->unavailable || this->window == nullptr
        || !this->window->isExposed() || !this->graph->isVisible())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (!device.hasOwner())
        return;

    void *native_handle = reinterpret_cast<void *>(this->window->winId());
#if defined(AOWIS_BGFX_NATIVE_WAYLAND_PRIVATE)
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))
    {
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        native_handle = native
            ? native->nativeResourceForWindow(QByteArrayLiteral("surface"), this->window)
            : nullptr;
    }
#endif
    if (native_handle == nullptr)
        return; // Retry when the platform exposes its native child surface.

    const BgfxFrameCoordinator::ViewRange views = device.frames().reserve(this, 1);
    if (!views.valid())
        return;
    const QSize pixels = targetPixels();
    if (!device.targets().attachWindow(this, views.first, native_handle,
                                       targetDimension(pixels.width()),
                                       targetDimension(pixels.height()), true))
    {
        device.frames().release(this);
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: could not create native swapchain; using QPainter";
        this->graph->update();
        return;
    }
    if (!this->gpu.initialize())
    {
        this->gpu.release();
        device.targets().release(this);
        device.frames().release(this);
        this->unavailable = true;
        this->retry_timer.stop();
        this->container->hide();
        qWarning() << "Projects bgfx viewport: GPU HUD shader unavailable; using QPainter";
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
    if (device.hasOwner() && device.frames().views(this).valid())
    {
        if (this->gpu_initialized)
            this->gpu.release();
        device.targets().release(this);
        device.frames().release(this);
    }
    else
    {
        // bgfx::shutdown() already destroyed all resources. Never call
        // bgfx::destroy() with handles from an old device generation.
        this->gpu = MapBgfxHudGpu();
        this->atlas = MapBgfxHudTextCache(2048);
    }
    this->gpu_initialized = false;
    this->attached = false;
    this->allocated_pixels = QSize();
    this->view_id = 0;
}

void ProjectGraphBgfxViewport::render()
{
    if (!this->attached || !this->graph->isVisible() || this->window == nullptr
        || !this->window->isExposed())
        return;
    BgfxDeviceManager &device = BgfxDeviceManager::shared();
    if (!device.hasOwner() || !device.frames().views(this).valid())
    {
        detach();
        if (this->container != nullptr)
            this->container->hide();
        this->graph->update();
        return;
    }
    const QSize pixels = targetPixels();
    if (pixels != this->allocated_pixels)
    {
        if (!device.targets().resizeWindow(this, targetDimension(pixels.width()),
                                            targetDimension(pixels.height())))
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
    QVector<ProjectGraphBgfxScene::Fold> folds;
    folds.reserve(this->graph->omissions.size());
    for (const ProjectRevisionGraph::Omission &omission : this->graph->omissions)
        folds.append({omission.visible_parent_id, omission.visible_child_id,
                      omission.position, omission.revision_count});

    ProjectGraphBgfxScene::build(&this->canvas, &this->atlas,
                                 this->graph->nodes, folds, view);
    // Qt's text atlas may evict old sprites while building a large scene.
    // Rebuild once with fresh texel references to avoid stale card labels.
    if (this->atlas.evictedSinceFrameStart())
        ProjectGraphBgfxScene::build(&this->canvas, &this->atlas,
                                     this->graph->nodes, folds, view);
    bgfx::setViewRect(this->view_id, 0, 0,
                      targetDimension(pixels.width()), targetDimension(pixels.height()));
    bgfx::setViewMode(this->view_id, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(this->view_id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x121820ff);
    bgfx::setViewTransform(this->view_id, nullptr, nullptr);
    bgfx::touch(this->view_id);
    this->gpu.uploadAtlas(&this->atlas);
    this->gpu.draw(this->view_id, this->canvas, QSizeF(this->graph->size()));
    // The Maps viewport retains its view reservation when its tab is hidden.
    // present() deliberately refuses multiple registered clients, so the
    // graph must submit its already-queued GPU commands through the shared
    // multi-client entry point. A global bgfx frame does not require every
    // reserved view to draw on every tick (hidden maps remain idle).
    if (!device.frames().renderAndPresent({{this, {}}}))
        qWarning() << "Projects bgfx viewport: frame coordinator refused submission";
}

bool ProjectGraphBgfxViewport::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != this->window)
        return QObject::eventFilter(watched, event);
    if (event->type() == QEvent::PlatformSurface)
    {
        QPlatformSurfaceEvent *surface_event = static_cast<QPlatformSurfaceEvent *>(event);
        if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        {
            detach();
            return false;
        }
    }
    if (event->type() == QEvent::Expose || event->type() == QEvent::Resize)
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
            // The child fills the parent graph. Mouse and wheel coordinates
            // therefore match, and its existing selection/inertia handlers
            // remain the *only* source of interaction semantics.
            QCoreApplication::sendEvent(this->graph, event);
            return true;
        default:
            return QObject::eventFilter(watched, event);
    }
}
