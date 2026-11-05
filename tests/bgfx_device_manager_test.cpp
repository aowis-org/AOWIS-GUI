#include "map/bgfx/bgfx_device_manager.h"

#include <cstdio>
#include <vector>
#include <stdexcept>

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

    // A live secondary must retire its views before the primary can destroy
    // the device. Retirement runs newest-first and is safe when callbacks
    // unregister themselves during iteration.
    int multi_starts = 0;
    int multi_stops = 0;
    int owner = 31;
    int client_a = 32;
    int client_b = 33;
    std::vector<int> retirement_order;
    BgfxDeviceManager multiple(
        [&multi_starts](const bgfx::Init &) { ++multi_starts; return true; },
        [&multi_stops]() { ++multi_stops; });
    check(multiple.initialize(&owner, settings, []() { return true; }, []() {}),
          "multi-client primary initialized");
    const BgfxDeviceManager::Generation generation_one = multiple.generation();
    check(generation_one != 0 && multiple.isCurrent(&owner, generation_one),
          "owner lease belongs to first device generation");
    check(!multiple.registerClient(&client_a, generation_one + 1, []() {})
          && !multiple.registerClient(&owner, generation_one, []() {}),
          "wrong-generation and duplicate owner leases refused");
    check(multiple.frames().reserve(&owner, 5).valid(), "owner reserves five views");
    check(multiple.registerClient(&client_a, generation_one,
                                  [&multiple, &client_a, &generation_one,
                                   &retirement_order]()
                                  {
                                      retirement_order.push_back(1);
                                      multiple.frames().release(&client_a);
                                      check(multiple.unregisterClient(&client_a, generation_one),
                                            "first secondary can unregister after releasing view");
                                  }), "first secondary registered");
    check(multiple.frames().reserve(&client_a, 1).first == 5,
          "first secondary obtains independent view");
    check(multiple.registerClient(&client_b, generation_one,
                                  [&multiple, &client_b, &owner, &generation_one,
                                   &retirement_order]()
                                  {
                                      retirement_order.push_back(2);
                                      check(!multiple.shutdown(&owner),
                                            "reentrant shutdown rejected during retirement");
                                      check(!multiple.unregisterClient(&client_b, generation_one),
                                            "live GPU reservation blocks unregister");
                                      multiple.frames().release(&client_b);
                                      check(multiple.unregisterClient(&client_b, generation_one),
                                            "second secondary unregisters after release");
                                  }), "second secondary registered");
    check(multiple.frames().reserve(&client_b, 1).first == 6,
          "second secondary obtains its own view");
    check(!multiple.registerClient(&client_a, generation_one, []() {}),
          "duplicate secondary lease refused");
    check(multiple.secondaryClientCount() == 2, "two live secondary leases counted");
    check(multiple.shutdown(&owner) && multi_stops == 1,
          "owner shutdown retires both secondary GPU clients first");
    check(retirement_order.size() == 2 && retirement_order[0] == 2
          && retirement_order[1] == 1,
          "clients retire in reverse registration order");
    check(!multiple.hasOwner() && multiple.secondaryClientCount() == 0
          && multiple.generation() == 0 && !multiple.frames().views(&client_a).valid(),
          "completed shutdown clears all leases and reservations");

    // An uncooperative GPU client must NOT be invalidated by a device reset.
    bool retire_allowed = false;
    int stubborn = 34;
    check(multiple.initialize(&owner, settings, []() { return false; },
                              [&multiple, &owner]() { multiple.shutdown(&owner); }),
          "second device generation starts");
    const BgfxDeviceManager::Generation generation_two = multiple.generation();
    check(generation_two != generation_one && !multiple.isCurrent(&client_a, generation_one),
          "recreated device rejects old client generation");
    check(!multiple.registerClient(&client_a, generation_one, []() {}),
          "stale registration cannot attach to recreated device");
    check(multiple.registerClient(&stubborn, generation_two,
                                  [&multiple, &stubborn, &generation_two, &retire_allowed]()
                                  {
                                      if (!retire_allowed)
                                          return;
                                      multiple.frames().release(&stubborn);
                                      multiple.unregisterClient(&stubborn, generation_two);
                                  }), "uncooperative client registered");
    check(multiple.frames().reserve(&stubborn, 1).valid(), "uncooperative view reserved");
    check(!multiple.shutdown(&owner) && multiple.owns(&owner) && multi_stops == 1,
          "shutdown refuses to destroy GPU handles still held by a client");
    check(multiple.isCurrent(&stubborn, generation_two)
          && multiple.frames().views(&stubborn).valid(),
          "failed shutdown preserves both device generation and GPU resources");
    check(!multiple.prepareFor(&client_a) && multiple.owns(&owner),
          "hidden owner cannot be stolen while a client refuses retirement");
    retire_allowed = true;
    check(multiple.shutdown(&owner) && multi_stops == 2,
          "shutdown recovers after client becomes retireable");

    // View reservations bypassing registration are still caught by the
    // shutdown barrier, preventing accidental unmanaged resource destruction.
    check(multiple.initialize(&owner, settings, []() { return true; }, []() {}),
          "third device generation starts");
    check(multiple.frames().reserve(&client_b, 1).valid(),
          "unregistered consumer can be detected from frame reservations");
    check(!multiple.shutdown(&owner) && multi_stops == 2,
          "unregistered live view blocks shutdown rather than being discarded");
    check(multiple.frames().release(&client_b), "unregistered view explicitly freed");
    check(multiple.shutdown(&owner) && multi_stops == 3,
          "owner may shut down after unmanaged view is released");

    // A throwing retirement callback must not leave the manager stuck in
    // reentrant-retirement mode. The owner remains active and can retry.
    check(multiple.initialize(&owner, settings, []() { return true; }, []() {}),
          "fourth device generation starts");
    const BgfxDeviceManager::Generation generation_four = multiple.generation();
    bool should_throw = true;
    check(multiple.registerClient(&client_a, generation_four,
                                  [&multiple, &client_a, &generation_four, &should_throw]()
                                  {
                                      if (should_throw)
                                          throw std::runtime_error("retirement failed");
                                      multiple.unregisterClient(&client_a, generation_four);
                                  }), "throwing client registered");
    check(!multiple.shutdown(&owner) && multiple.owns(&owner) && multi_stops == 3,
          "exception from client retire callback does not destroy device");
    should_throw = false;
    check(multiple.shutdown(&owner) && multi_stops == 4,
          "device can shut down after retrying formerly throwing client");
    check(multi_starts == 4, "multi-client device initialized exactly four times");

    // Check real target lifetime ordering under the manager's retirement
    // barrier using an injectable mock framebuffer backend (no GPU needed).
    std::vector<int> target_events;
    int surface_owner = 41;
    int surface_client = 42;
    int unmanaged_target = 43;
    int native_window_a = 44;
    int native_window_b = 45;
    std::uint16_t next_target = 100;
    BgfxDeviceManager targets(
        [](const bgfx::Init &) { return true; },
        [&target_events]() { target_events.push_back(9); },
        [&next_target, &target_events](void *, std::uint16_t, std::uint16_t)
        {
            target_events.push_back(1);
            return bgfx::FrameBufferHandle{next_target++};
        },
        [&target_events](std::uint16_t, bgfx::FrameBufferHandle)
        {
            target_events.push_back(2);
        },
        [&target_events](bgfx::FrameBufferHandle)
        {
            target_events.push_back(3);
        });
    check(targets.initialize(&surface_owner, settings, []() { return true; }, []() {}),
          "target ordering test device starts");
    check(targets.frames().reserve(&surface_owner, 5).valid()
          && targets.targets().attachDefault(&surface_owner, 0),
          "primary framebuffer bound");
    const BgfxDeviceManager::Generation target_generation = targets.generation();
    check(targets.registerClient(&surface_client, target_generation,
                                 [&targets, &surface_client, &target_generation,
                                  &target_events]()
                                 {
                                     target_events.push_back(4);
                                     targets.targets().release(&surface_client);
                                     targets.frames().release(&surface_client);
                                     check(targets.unregisterClient(&surface_client,
                                                                   target_generation),
                                           "secondary target lease released");
                                 }), "secondary target owner registered");
    check(targets.frames().reserve(&surface_client, 1).first == 5
          && targets.targets().attachWindow(&surface_client, 5, &native_window_a,
                                            500, 350, true),
          "secondary framebuffer created and bound");
    target_events.clear();
    check(targets.shutdown(&surface_owner),
          "primary shutdown retires secondary GPU framebuffer");
    check(target_events.size() == 5 && target_events[0] == 4
          && target_events[1] == 2 && target_events[2] == 3
          && target_events[3] == 2 && target_events[4] == 9,
          "retire callback, unbind, destroy, primary unbind, bgfx shutdown ordered");
    check(!targets.isCurrent(&surface_client, target_generation)
          && !targets.targets().contains(&surface_client),
          "old target and generation cannot be reused");

    // A stray target without a registered lease must also be refused.
    check(targets.initialize(&surface_owner, settings, []() { return true; }, []() {}),
          "new device after framebuffer teardown starts");
    check(targets.targets().attachDefault(&surface_owner, 0),
          "new default framebuffer attached");
    check(targets.targets().attachWindow(&unmanaged_target, 7, &native_window_b,
                                         320, 240, true),
          "unmanaged secondary framebuffer attached for guard test");
    check(!targets.shutdown(&surface_owner) && targets.hasOwner(),
          "unmanaged native framebuffer prevents device shutdown");
    check(targets.targets().release(&unmanaged_target),
          "unmanaged framebuffer explicitly destroyed");
    check(targets.shutdown(&surface_owner),
          "device shutdown succeeds after orphan framebuffer cleanup");

    if (failures == 0)
        std::puts("bgfx process-global device ownership lifecycle passed");
    return failures == 0 ? 0 : 1;
}
