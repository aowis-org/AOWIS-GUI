#include "map/bgfx/bgfx_device_manager.h"

#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main()
{
    int starts = 0;
    int stops = 0;
    int retired = 0;
    bool init_success = false;
    bool first_visible = true;
    bool second_visible = true;
    int first = 1;
    int second = 2;
    int third = 3;

    BgfxDeviceManager manager(
        [&starts, &init_success](const bgfx::Init &)
        {
            ++starts;
            return init_success;
        },
        [&stops]() { ++stops; });
    bgfx::Init settings{};

    check(!manager.prepareFor(nullptr), "null requester must not acquire");
    check(!manager.owns(nullptr) && !manager.hasOwner(), "device begins unowned");
    check(manager.prepareFor(&first), "first requester admitted");
    check(!manager.initialize(&first, settings, [&first_visible]() { return first_visible; },
                               [&manager, &first]() { manager.shutdown(&first); }),
          "failed GPU initialization is reported");
    check(!manager.hasOwner() && starts == 1 && stops == 0,
          "failed initialization never claims or shuts down device");

    init_success = true;
    check(manager.initialize(&first, settings, [&first_visible]() { return first_visible; },
                             [&manager, &first, &retired]()
                             {
                                 ++retired;
                                 manager.shutdown(&first);
                             }),
          "successful GPU initialization claims device");
    check(manager.owns(&first) && manager.hasOwner(), "first surface owns device");
    check(manager.frames().reserve(&first, 5).first == 0,
          "first initialized map gets a contiguous five-view block");
    check(manager.prepareFor(&first), "existing owner may continue");
    check(!manager.initialize(&second, settings, []() { return true; }, []() {}),
          "cannot initialize second device while first owns it");
    check(!manager.prepareFor(&second) && retired == 0,
          "visible owner cannot be displaced");
    check(!manager.shutdown(&second) && stops == 0,
          "non-owner cannot shut down the device");

    first_visible = false;
    check(manager.prepareFor(&second), "hidden owner relinquishes the device");
    check(retired == 1 && stops == 1 && !manager.hasOwner(),
          "takeover callback releases exactly once");
    check(!manager.frames().views(&first).valid(),
          "device shutdown invalidates all prior view reservations");
    check(manager.initialize(&second, settings, [&second_visible]() { return second_visible; },
                             [&manager, &second]() { manager.shutdown(&second); }),
          "second surface initializes after handoff");
    check(manager.frames().reserve(&second, 5).first == 0,
          "reinitialized device can reuse view 0");
    check(!manager.prepareFor(&third), "second visible owner blocks third");
    second_visible = false;
    check(manager.prepareFor(&third) && stops == 2,
          "second hidden owner releases for third");

    // A buggy retire callback must never permit a second simultaneous device.
    bool third_visible = false;
    check(manager.initialize(&third, settings, [&third_visible]() { return third_visible; },
                             []() {}), "third owns device");
    check(!manager.prepareFor(&first) && manager.owns(&third),
          "takeover fails if retire did not actually release the device");
    check(manager.shutdown(&third) && !manager.hasOwner(), "explicit owner shutdown");
    check(!manager.shutdown(&third) && stops == 3, "repeated shutdown is a no-op");
    check(starts == 4, "one initial failure and three successful starts");

    // Projects-first startup: the graph is a legitimate primary window,
    // not a client that must wait for Maps to initialize bgfx. Its one-view
    // allocation must be fully retired before the hidden graph hands the
    // process-global device to a newly opened five-view map.
    int graph_starts = 0;
    int graph_stops = 0;
    bool graph_visible = true;
    int projects_graph = 11;
    int map_surface = 12;
    BgfxDeviceManager projects_first(
        [&graph_starts](const bgfx::Init &) { ++graph_starts; return true; },
        [&graph_stops]() { ++graph_stops; });
    check(projects_first.prepareFor(&projects_graph),
          "Projects can request a device with no Maps client");
    check(projects_first.initialize(&projects_graph, settings,
                                    [&graph_visible]() { return graph_visible; },
                                    [&projects_first, &projects_graph]()
                                    {
                                        projects_first.shutdown(&projects_graph);
                                    }),
          "Projects first initializes its own primary bgfx device");
    check(projects_first.frames().reserve(&projects_graph, 1).first == 0,
          "Projects gets view zero on its initial default swapchain");
    check(!projects_first.prepareFor(&map_surface),
          "visible Projects primary cannot be stolen by Maps");
    graph_visible = false;
    check(projects_first.prepareFor(&map_surface),
          "hidden Projects primary can retire for Maps");
    check(graph_stops == 1 && !projects_first.frames().views(&projects_graph).valid(),
          "handoff destroys the Projects device generation and view IDs");
    check(projects_first.initialize(&map_surface, settings, []() { return true; },
                                    [&projects_first, &map_surface]()
                                    {
                                        projects_first.shutdown(&map_surface);
                                    }),
          "Maps initializes after the hidden Projects device retires");
    check(projects_first.frames().reserve(&map_surface, 5).first == 0,
          "Maps obtains its original five-view block after Projects");
    check(projects_first.shutdown(&map_surface) && graph_starts == 2 && graph_stops == 2,
          "Projects-to-Maps ownership lifecycle balances init and shutdown");

    if (failures == 0)
        std::puts("bgfx process-global device ownership lifecycle passed");
    return failures == 0 ? 0 : 1;
}
