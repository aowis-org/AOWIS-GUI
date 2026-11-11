cmake_minimum_required(VERSION 3.21)
project(AOWIS-SERVER-GUI LANGUAGES CXX)

include(CTest)

# The shared renderer must have a real Git submodule pin, and no shadow
# headers/shaders must remain in the GUI after extraction.
if(BUILD_TESTING AND UNIX AND NOT EMSCRIPTEN)
    add_test(NAME aowis-render-extraction-boundary
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_render_extraction_boundary.sh")
    set_tests_properties(aowis-render-extraction-boundary PROPERTIES LABELS "aowis")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Widgets
    Network
    Sql
    OPTIONAL_COMPONENTS
    GuiPrivate
)

set(AOWIS_HAS_WAYLAND_POINTER_LOCK 0)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN
   AND Qt6_VERSION VERSION_GREATER_EQUAL 6.5
   AND TARGET Qt6::GuiPrivate)
    # Qt GuiPrivate (also needed by bgfx for native handles). Use Qt's public
    # Wayland application native interface plus the Wayland client runtime
    # directly; do not require the separately packaged Qt WaylandClient
    # development module or system wayland-protocols XML files.
    set(AOWIS_HAS_WAYLAND_POINTER_LOCK 1)
endif()

if(EMSCRIPTEN)
    include(CheckCXXSourceCompiles)

    set(CMAKE_REQUIRED_LIBRARIES Qt6::Core)
    check_cxx_source_compiles(
        "#include <QtCore/qglobal.h>
#if !QT_CONFIG(thread)
#error Qt WebAssembly thread support is disabled
#endif
int main() { return 0; }"
        AOWIS_QT_WASM_HAS_THREAD_SUPPORT
    )
    unset(CMAKE_REQUIRED_LIBRARIES)

    if(NOT AOWIS_QT_WASM_HAS_THREAD_SUPPORT)
        message(FATAL_ERROR
            "The WebAssembly build requires a thread-enabled Qt WebAssembly kit. "
            "Install/use the Qt 'WebAssembly (multi-threaded)' component or build Qt "
            "for wasm with -feature-thread."
        )
    endif()
endif()

if(NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS
        Positioning
        SerialPort
    )
endif()

# The Go bridge currently builds for its host. Keep cross builds on Qt HTTP.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-NETWORK/CMakeLists.txt")
        message(FATAL_ERROR "Missing AOWIS-NETWORK sources. Run ./git_submodule_init.sh --network and verify the GUI gitlink.")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Concurrent)
    function(aowis_add_gui_network)
        set(BUILD_TESTING OFF)
        add_subdirectory(external/AOWIS-NETWORK EXCLUDE_FROM_ALL)
    endfunction()
    aowis_add_gui_network()
    if(NOT TARGET AOWIS::NetworkQt OR NOT AOWIS_NETWORK_WITH_WIREGUARD)
        message(FATAL_ERROR "Native Linux GUI requires the NETWORK Qt adapter and WireGuard engine.")
    endif()
endif()

# Configuration storage (AOWIS-CONFIG) on every platform, including WASM:
# native POSIX files, browser localStorage. Its tests are not built here.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-CONFIG/CMakeLists.txt")
    message(FATAL_ERROR "Missing AOWIS-CONFIG sources. Run ./git_submodule_init.sh --config and verify the GUI gitlink.")
endif()
add_subdirectory(external/AOWIS-CONFIG EXCLUDE_FROM_ALL)

# GeographicLib submodule
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_BOTH_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(BUILD_MANPAGES OFF CACHE BOOL "" FORCE)
set(USE_BOOST OFF CACHE BOOL "" FORCE)

add_subdirectory(external/GeographicLib EXCLUDE_FROM_ALL)

# AOWIS-SERVER-MODEL submodule
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/CMakeLists.txt")
    message(FATAL_ERROR
        "AOWIS-SERVER-MODEL submodule is missing. Run:\n"
        "  git submodule update --init --recursive"
    )
endif()
add_subdirectory(external/AOWIS-SERVER-MODEL EXCLUDE_FROM_ALL)
# Embed the checksum of the actual MODEL sources in every GUI target. Re-run
# CMake automatically when a codec source or header changes, even if uncommitted.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ModelCodecFingerprint.cmake")
aowis_generate_model_codec_fingerprint(
    "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
    "${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/model-fingerprint")


# AOWIS-SERVER-EPANET submodule
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-EPANET/CMakeLists.txt")
    message(FATAL_ERROR
        "AOWIS-SERVER-EPANET submodule is missing. Run:\n"
        "  git submodule update --init --recursive"
    )
endif()
set(AOWIS_SERVER_EPANET_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(AOWIS_SERVER_EPANET_BUILD_SHARED_EPANET OFF CACHE BOOL "" FORCE)
add_subdirectory(external/AOWIS-SERVER-EPANET EXCLUDE_FROM_ALL)

# The GUI uses the submodule's pinned commit by default, never a sibling
# checkout picked up incidentally. An intentional CMake override is permitted
# for renderer development and must be specified explicitly by the caller.
set(AOWIS_RENDER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-GUI-Render"
    CACHE PATH "AOWIS-GUI-Render source checkout (normally the pinned submodule)")
if(NOT EXISTS "${AOWIS_RENDER_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "AOWIS-GUI-Render missing at ${AOWIS_RENDER_SOURCE_DIR}. "
        "Run ./git_submodule_init.sh --render or initialize the pinned Git submodule.")
endif()
# Detect incomplete Render Git checkouts at configure time, before the
# compiler encounters a missing header or the linker sees an absent object.
foreach(_aowis_render_required_file IN ITEMS
        src/map/bgfx/map_bgfx_globe_terrain_gpu.h
        src/map/bgfx/map_bgfx_globe_terrain_gpu.cpp
        src/map/bgfx/map_bgfx_globe_vertex_layouts.h
        src/map/bgfx/map_bgfx_globe_vertex_layouts.cpp
        src/map/render/map_globe_flow_geometry.h
        src/map/render/map_globe_flow_geometry.cpp
        src/map/render/map_globe_underground_geometry.h
        src/map/render/map_globe_underground_geometry.cpp
        src/map/render/map_globe_scene_sync.h
        src/map/render/map_globe_scene_sync.cpp
        src/geo/geo_wgs84_ellipsoid.h
        src/geo/geo_wgs84_ellipsoid.cpp
        src/map/data/map_terrain_tile.h
        src/map/data/map_terrain_tile.cpp
        src/map/core/map_model.h
        src/map/core/map_model.cpp
        src/map/core/map_imagery_provider_directory.h
        src/map/data/map_terrain_tile_source.h
        src/map/render/map_globe_surface_preparation.h
        src/map/render/map_globe_surface_preparation.cpp
        src/services/interface_server_map.h
        src/map/data/map_server_transport.h
        src/map/data/map_tile_repository.cpp
        src/map/data/map_terrain_repository.cpp
        src/map/core/map_network_types.h
        src/network/network_render_snapshot.h
        src/map/render/map_globe_network_scene.cpp
        src/map/bgfx/map_bgfx_globe_network.cpp
        src/map/bgfx/map_bgfx_globe_surface.cpp
        src/map/bgfx/map_bgfx_hud.cpp
        src/map/bgfx/map_bgfx_map_view.cpp
        src/map/bgfx/bgfx_qt_native_surface.cpp
        src/services/interface_server_map_rest.cpp
        src/map/core/map_provider_catalog.cpp
        cmake/aowis_render_dependencies.cmake)
    if(NOT EXISTS "${AOWIS_RENDER_SOURCE_DIR}/${_aowis_render_required_file}")
        message(FATAL_ERROR
            "AOWIS-GUI-Render checkout is incomplete: ${_aowis_render_required_file} is missing at ${AOWIS_RENDER_SOURCE_DIR}.\n"
            "Run ./git_submodule_update.sh --render to restore tracked Render sources before building or testing.")
    endif()
endforeach()
set(AOWIS_RENDER_VENDOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external" CACHE PATH
    "Pinned bx/bimg/bgfx sources for the shared renderer")
set(AOWIS_RENDER_BUILD_MAP_CORE ON CACHE BOOL "" FORCE)
# Geodesy and terrain decoding live in Render. GeographicLib_STATIC and
# AOWIS::Model already exist at this point, so Render links the GUI's copies
# and never builds its own submodules inside the GUI.
set(AOWIS_RENDER_BUILD_GEO ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_TERRAIN_DATA ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_VIEW ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_GLOBE_TERRAIN ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_MAP_DATA ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_GLOBE_NETWORK ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_NETWORK_SCENE ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_GLOBE_MODELS ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_QT_SURFACE ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_MAP_REST ON CACHE BOOL "" FORCE)
set(AOWIS_RENDER_BUILD_MAP ON CACHE BOOL "" FORCE)
add_subdirectory("${AOWIS_RENDER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/aowis-render" EXCLUDE_FROM_ALL)
# Some GUI-only CPU regression executables compile renderer headers directly.
include_directories("${AOWIS_RENDER_SOURCE_DIR}/src")

# epanet-js is a SQLite source-file format. Keep its reader and converter in one
# internal target so the GUI and every test use the same implementation.
add_library(aowis-epanet-js-import STATIC
    src/import/epanet_js_project_reader.cpp
    src/import/epanet_js_project_reader.h
    src/import/epanet_js_project_converter.cpp
    src/import/epanet_js_project_converter.h
    src/import/epanet_js_validation.cpp
    src/import/epanet_js_validation.h
    src/import/epanet_js_conversion_common.h
    src/import/epanet_js_patterns_curves.cpp
    src/import/epanet_js_patterns_curves.h
    src/import/epanet_js_settings.cpp
    src/import/epanet_js_project_identity.cpp
    src/import/epanet_js_project_identity.h
    src/import/epanet_js_geometry.cpp
    src/import/epanet_js_geometry.h
    src/import/epanet_js_pipes.cpp
    src/import/epanet_js_pipes.h
    src/import/epanet_js_demand_points.cpp
    src/import/epanet_js_demand_points.h
    src/import/epanet_js_water_quality.cpp
    src/import/epanet_js_water_quality.h
    src/import/epanet_js_pumps_valves.cpp
    src/import/epanet_js_pumps_valves.h
    src/import/epanet_js_controls.cpp
    src/import/epanet_js_controls.h
    src/import/epanet_js_nodes.cpp
    src/import/epanet_js_nodes.h
    src/import/epanet_js_settings.h
    src/import/epanet_js_schema.cpp
    src/import/epanet_js_schema.h
    src/import/epanet_js_units.cpp
    src/import/epanet_js_units.h
    src/import/epanet_js_project_importer.cpp
    src/import/epanet_js_project_importer.h
)
target_include_directories(aowis-epanet-js-import
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(aowis-epanet-js-import
    PUBLIC
        Qt6::Core
        AOWIS::ServerModel
    PRIVATE
        Qt6::Sql
)

# Keep the .qm files embedded by translations.qrc synchronized with .ts.
# Native Qt LinguistTools provides a host lrelease (also usable when cross-
# compiling the GUI for WebAssembly). Without that tool, preserve the checked-
# in catalogues and warn rather than unexpectedly breaking older Qt kits.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
set(AOWIS_GUI_LRELEASE_COMMAND "")
if(TARGET Qt6::lrelease)
    set(AOWIS_GUI_LRELEASE_COMMAND "$<TARGET_FILE:Qt6::lrelease>")
else()
    find_program(AOWIS_GUI_LRELEASE_EXECUTABLE NAMES lrelease6 lrelease-qt6 lrelease)
    if(AOWIS_GUI_LRELEASE_EXECUTABLE)
        set(AOWIS_GUI_LRELEASE_COMMAND "${AOWIS_GUI_LRELEASE_EXECUTABLE}")
    endif()
endif()

if(AOWIS_GUI_LRELEASE_COMMAND)
    set(AOWIS_GUI_TRANSLATION_OUTPUTS "")
    foreach(AOWIS_GUI_LANGUAGE fr ar de es)
        foreach(AOWIS_GUI_TRANSLATION_DOMAIN gui render)
            set(AOWIS_GUI_TS
                "${CMAKE_CURRENT_SOURCE_DIR}/translations/aowis_${AOWIS_GUI_TRANSLATION_DOMAIN}_${AOWIS_GUI_LANGUAGE}.ts")
            set(AOWIS_GUI_QM
                "${CMAKE_CURRENT_SOURCE_DIR}/translations/aowis_${AOWIS_GUI_TRANSLATION_DOMAIN}_${AOWIS_GUI_LANGUAGE}.qm")
            add_custom_command(OUTPUT "${AOWIS_GUI_QM}"
                COMMAND "${AOWIS_GUI_LRELEASE_COMMAND}" "${AOWIS_GUI_TS}" -qm "${AOWIS_GUI_QM}"
                DEPENDS "${AOWIS_GUI_TS}" VERBATIM)
            list(APPEND AOWIS_GUI_TRANSLATION_OUTPUTS "${AOWIS_GUI_QM}")
        endforeach()
    endforeach()
    add_custom_target(aowis-gui-compiled-translations
        DEPENDS ${AOWIS_GUI_TRANSLATION_OUTPUTS})
else()
    message(WARNING "Qt lrelease is unavailable: translation catalogues will not be rebuilt from .ts. Install Qt LinguistTools to compile updated translations.")
endif()

qt_add_executable(aowis-server-gui
    resources.qrc
    translations/translations.qrc

    # Application
    src/app/builtin_examples.cpp
    src/app/builtin_examples.h
    src/app/footer_statusbar.cpp
    src/app/footer_statusbar.h
    src/app/history_dock.cpp
    src/app/history_dock.h
    src/config/gui_language.cpp
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/main_window.h
    src/app/menubar.cpp
    src/app/menubar.h
    src/app/top_control_bar.cpp
    src/app/top_control_bar.h

    # Common
    src/common/_enums_structs.h
    src/common/_sizes.h

    # Configuration
    src/config/gui_configuration.cpp
    src/config/gui_preferences.cpp
    src/config/gui_preferences.h
    src/config/unit_profile_storage.cpp
    src/config/gui_configuration.h
    src/config/map_server_client_configuration.cpp
    src/config/map_server_client_configuration.h
    src/config/project_server_profile_storage.cpp
    src/config/project_server_profile_storage.h
    src/config/gui_config_store.cpp
    src/config/gui_config_store.h
    src/config/wireguard_profile_storage.cpp
    src/config/wireguard_profile_storage.h
    src/config/personal_repository_storage.cpp
    src/config/personal_repository_storage.h
    src/config/shortcut_registry.cpp
    src/config/shortcut_registry.h


    # Network
    src/network/wireguard_runtime_status.cpp
    src/network/wireguard_runtime_status.h
    src/network/hydraulic_data.cpp
    src/network/hydraulic_data.h
    src/network/hydraulic_edit_transaction_capture.cpp
    src/network/hydraulic_edit_transaction_capture.h
    src/projects/personal_edit_queue.cpp
    src/projects/personal_edit_queue.h
    src/projects/hydraulic_draft_event_adapter.cpp
    src/projects/hydraulic_draft_event_adapter.h
    src/projects/hydraulic_draft_entity_registry.h
    src/projects/personal_draft_edit_stage.cpp
    src/projects/personal_draft_edit_stage.h
    src/projects/personal_history_navigation_intent_store.cpp
    src/projects/personal_history_navigation_intent_store.h
    src/projects/personal_draft_edit_session.cpp
    src/projects/personal_draft_edit_session.h
    src/network/hydraulic_entity_table_widget.cpp
    src/network/hydraulic_entity_table_widget.h
    src/network/hydraulic_network_editor.cpp
    src/network/hydraulic_network_editor.h
    src/network/network_render_snapshot_builder.cpp
    src/network/network_render_snapshot_builder.h
    src/network/network_symbology_values.h

    # Simulation
    src/simulation/simulation_diagnostics_widget.cpp
    src/simulation/simulation_diagnostics_widget.h
    src/simulation/simulation_manager.cpp
    src/simulation/simulation_manager.h
    src/simulation/simulation_statistics_dialog.cpp
    src/simulation/simulation_statistics_dialog.h

    # Settings
    src/settings/keyboard_shortcuts_settings_widget.cpp
    src/settings/keyboard_shortcuts_settings_widget.h
    src/settings/map_settings_widget.cpp
    src/settings/map_settings_widget.h
    src/settings/units_settings_widget.cpp
    src/settings/units_settings_widget.h
    src/settings/wireguard_vpn_settings_widget.cpp
    src/settings/wireguard_vpn_settings_widget.h

    # Tabs
    src/tabs/tab_alarms_widget.cpp
    src/tabs/tab_alarms_widget.h
    src/tabs/tab_demand_points_widget.cpp
    src/tabs/tab_demand_points_widget.h
    src/tabs/tab_customers_widget.cpp
    src/tabs/tab_customers_widget.h
    src/tabs/tab_energy_widget.cpp
    src/tabs/tab_energy_widget.h
    src/tabs/tab_junctions_widget.cpp
    src/tabs/tab_junctions_widget.h
    src/tabs/tab_logs_widget.cpp
    src/tabs/tab_logs_widget.h
    src/projects/repository_probe.cpp
    src/tabs/tab_projects_widget.cpp
    src/tabs/tab_projects_widget.h
    src/projects/project_source_reader.cpp
    src/projects/project_source_reader.h
    src/projects/project_source_writer.cpp
    src/projects/project_source_writer.h
    src/projects/personal_working_copy_client.cpp
    src/projects/personal_commit_intent_store.cpp
    src/projects/personal_commit_intent_store.h
    src/projects/personal_working_copy_client.h
    src/tabs/personal_working_copy_dialog.cpp
    src/tabs/personal_working_copy_dialog.h
    src/tabs/project_revision_graph.cpp
    src/tabs/project_revision_graph.h
    src/tabs/project_server_availability.h
    src/tabs/project_graph_bgfx_scene.cpp
    src/tabs/project_graph_bgfx_scene.h
    src/tabs/project_graph_bgfx_viewport.cpp
    src/tabs/project_graph_bgfx_viewport.h
    src/tabs/project_graph_render_policy.h
    src/tabs/project_graph_placement.h
    src/tabs/tab_map_workspace.cpp
    src/tabs/map_workspace_viewport_page.cpp
    src/tabs/tab_map_workspace.h
    src/tabs/map_workspace_mode.h
    src/tabs/map_mode_tool_panel.cpp
    src/tabs/map_mode_tool_panel.h
    src/tabs/tab_hydraulic_libraries_widget.cpp
    src/tabs/tab_hydraulic_libraries_widget.h
    src/tabs/tab_hydraulic_controls_widget.cpp
    src/tabs/tab_hydraulic_controls_widget.h
    src/tabs/tab_map_editor_container.cpp
    src/tabs/tab_map_editor_container.h
    src/tabs/tab_map_monitor_container.cpp
    src/tabs/tab_map_monitor_container.h
    src/tabs/tab_pipes_widget.cpp
    src/tabs/tab_pipes_widget.h
    src/tabs/tab_pumps_widget.cpp
    src/tabs/tab_pumps_widget.h
    src/tabs/tab_reservoirs_widget.cpp
    src/tabs/tab_reservoirs_widget.h
    src/tabs/tab_settings_widget.cpp
    src/tabs/tab_settings_widget.h
    src/tabs/tab_tanks_widget.cpp
    src/tabs/tab_tanks_widget.h
    src/tabs/tab_valves_widget.cpp
    src/tabs/tab_valves_widget.h

    # Services
    src/services/map_server_transport_factory.cpp
    src/services/map_server_transport_factory.h

    # Map core
    src/map/core/gui_map_provider_catalog.cpp
    src/map/core/gui_map_provider_catalog.h
    src/map/core/map_models.h
    src/map/core/map_navigation_widget.cpp
    src/map/core/map_navigation_widget.h
    src/map/core/map_scale_renderer.h
    src/map/core/map_wayland_pointer_lock.cpp
    src/map/core/map_wayland_pointer_lock.h
    src/map/core/map_widget.cpp
    src/map/core/map_widget.h

    # Map editor
    src/map/editor/map_canvas_devicelinks.cpp
    src/map/editor/map_canvas_devicelinks.h
    src/map/editor/map_canvas_entities.cpp
    src/map/editor/map_canvas_entities.h
    src/map/editor/map_canvas_markers.cpp
    src/map/editor/map_canvas_markers.h
    src/map/editor/map_canvas_hit_box.h
    src/map/editor/map_canvas_pipes.cpp
    src/map/editor/map_canvas_pipes.h
    src/map/editor/map_canvas_projection.h
    src/map/editor/map_canvas_placement.cpp
    src/map/editor/map_canvas_placement.h
    src/map/editor/map_canvas_selection.cpp
    src/map/editor/map_canvas_selection.h
    src/map/editor/map_canvas_widget.cpp
    src/map/editor/map_canvas_widget.h
    src/map/editor/map_editor_controller.cpp
    src/map/editor/map_editor_controller.h
    src/map/editor/map_editor_guide_dock.cpp
    src/map/editor/map_editor_guide_dock.h
    src/map/editor/map_editor_visual_state.h

    # Map rendering
    src/map/render/map_entity_pixmap_renderer.cpp
    src/map/render/map_entity_pixmap_renderer.h

    # Resolved network symbology from the hydraulic data (the scene and
    # the symbology data itself are Render's AOWIS::RenderGlobeNetwork)
    src/map/render/map_network_symbology_resolver.h
    src/map/render/map_network_symbology_resolver.cpp

    # Entity inspectors
    src/entity_inspector/entity_inspector_demand_point.cpp
    src/entity_inspector/entity_inspector_demand_point.h
    src/entity_inspector/entity_inspector_dock.cpp
    src/entity_inspector/entity_inspector_dock.h
    src/entity_inspector/entity_inspector_junction.cpp
    src/entity_inspector/entity_inspector_junction.h
    src/entity_inspector/entity_inspector_pipe.cpp
    src/entity_inspector/entity_inspector_pipe.h
    src/entity_inspector/entity_inspector_pump.cpp
    src/entity_inspector/entity_inspector_pump.h
    src/entity_inspector/entity_inspector_reservoir.cpp
    src/entity_inspector/entity_inspector_reservoir.h
    src/entity_inspector/entity_inspector_tank.cpp
    src/entity_inspector/entity_inspector_tank.h
    src/entity_inspector/entity_inspector_valve.cpp
    src/entity_inspector/entity_inspector_valve.h
    src/entity_inspector/entity_inspector_widget.cpp
    src/entity_inspector/entity_inspector_widget.h
    src/entity_inspector/entity_map_legend_dock.cpp
    src/entity_inspector/entity_map_legend_dock.h

    # Widgets
    src/widgets/combo_checkboxes.cpp
    src/widgets/combo_checkboxes.h
    src/widgets/group_box_collapsible.cpp
    src/widgets/group_box_collapsible.h
    src/widgets/main_navigation_widget.cpp
    src/widgets/main_navigation_widget.h
    src/widgets/slider_number_control.cpp
    src/widgets/slider_number_control.h
    src/widgets/wasm_popup_menu.cpp
    src/widgets/wasm_popup_menu.h
)

if(APPLE)
    set_target_properties(aowis-server-gui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "AOWIS Controller [Standalone]"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.aowis.controller"
    )
endif()

if(AOWIS_STANDALONE_BUILD)
    target_sources(aowis-server-gui
        PRIVATE
            src/services/interface_server_map_standalone.h
            src/services/interface_server_map_standalone.cpp
    )
    
    target_compile_definitions(aowis-server-gui
        PRIVATE
            AOWIS_STANDALONE
    )
endif()

target_compile_definitions(aowis-server-gui
    PRIVATE
        AOWIS_HAS_WAYLAND_POINTER_LOCK=${AOWIS_HAS_WAYLAND_POINTER_LOCK}
)

if(AOWIS_HAS_WAYLAND_POINTER_LOCK)
    # qpa/qplatformnativeinterface.h (the wl_surface of a window).
    target_link_libraries(aowis-server-gui PRIVATE Qt6::GuiPrivate)
    message(STATUS "AOWIS Wayland relative-pointer orbit support: enabled (runtime protocol binding)")
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    message(STATUS "AOWIS Wayland relative-pointer orbit support: unavailable (Qt 6.5+ GuiPrivate required)")
endif()

target_include_directories(aowis-server-gui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(EMSCRIPTEN)
    # WebAssembly SIMD (128 bit): clang auto-vectorizes the app's hot loops
    # (vertex generation of the map, editor overlay, labels and HUD, tile
    # format handling) into wasm SIMD instead of scalar code. Every browser
    # with the WebGL 2 the map requires also runs wasm SIMD (Chrome/Edge 91,
    # Firefox 89, Safari 16.4); a browser without it refuses to load the
    # module at all, so ./compile_wasm.sh accepts AOWIS_WASM_SIMD=OFF from
    # the environment for such a deployment. Only this target: Qt and bx
    # are left as they were built (no app source includes bx's SIMD
    # headers, so the types cannot disagree between translation units).
    option(AOWIS_WASM_SIMD "Compile the app with WebAssembly SIMD (-msimd128)" ON)
    if(AOWIS_WASM_SIMD)
        target_compile_options(aowis-server-gui PRIVATE "-msimd128")
        target_link_options(aowis-server-gui PRIVATE "-msimd128")
    endif()
endif()

target_link_libraries(aowis-server-gui
    PRIVATE
        Qt6::Widgets
        Qt6::Network
        Qt6::Sql
        aowis::config
        aowis-epanet-js-import
        AOWIS::ServerModel
        AOWIS::ServerEpanet
        AOWIS::RenderGeo
        AOWIS::RenderTerrainData
        AOWIS::RenderView
        AOWIS::RenderGlobeTerrain
        AOWIS::RenderMapData
        AOWIS::RenderGlobeNetwork
        GeographicLib_STATIC
)

target_compile_definitions(aowis-server-gui PRIVATE AOWIS_PROJECTS_BGFX_BRIDGE=1)
target_sources(aowis-server-gui PRIVATE
    src/map/bgfx/map_bgfx_legend_content.h
    src/map/bgfx/map_bgfx_legend_content.cpp
    src/map/bgfx/map_bgfx_editor_overlay.h
    src/map/bgfx/map_bgfx_editor_overlay.cpp
    src/map/bgfx/map_bgfx_editor_icons.h
    src/map/bgfx/map_bgfx_editor_icons.cpp
    src/map/bgfx/map_bgfx_globe_model_images.h
    src/map/bgfx/map_bgfx_globe_model_images.cpp
    src/map/bgfx/map_bgfx_globe_hud_controller.h
    src/map/bgfx/map_bgfx_globe_hud_controller.cpp
    src/map/bgfx/map_bgfx_surface.h
    src/map/bgfx/map_bgfx_surface.cpp)
target_link_libraries(aowis-server-gui PRIVATE AOWIS::RenderCore AOWIS::RenderGeometry AOWIS::RenderGlobe
    AOWIS::RenderMap AOWIS::RenderQtSurface AOWIS::RenderMapRest)
if(EMSCRIPTEN)
    # Keep wasm function names in the binary (name section only, no
    # DWARF): browser stack traces show "bgfx::gl::...::init" instead of
    # "$func25562". Costs a few MB of download, so it is off now that the
    # browser renderer is stable; ./compile_wasm.sh passes
    # AOWIS_WASM_FUNCTION_NAMES=ON from the environment for debugging.
    option(AOWIS_WASM_FUNCTION_NAMES
        "Keep WebAssembly function names for readable browser stack traces" OFF)
    if(AOWIS_WASM_FUNCTION_NAMES)
        target_link_options(aowis-server-gui PRIVATE "--profiling-funcs")
    endif()
endif()
# The Emscripten version shim reaches this target through bgfx -> bx.

if(EMSCRIPTEN)
    # The map needs WebGL 2 / OpenGL ES 3 (bgfx's GLES 3 renderer and its
    # essl shaders). Requiring it for every context also drops Emscripten's
    # WebGL 1 code paths; no part of the application uses WebGL 1.
    target_link_options(aowis-server-gui PRIVATE
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )

    # Qt's default 50 MB initial wasm memory is too small for the linked
    # Qt Widgets + bgfx application and its embedded resources. Use Qt's
    # own target property (rather than a competing -sINITIAL_MEMORY flag)
    # to allocate 256 MiB; the toolchain's heap growth remains enabled.
    set_target_properties(aowis-server-gui PROPERTIES
        QT_WASM_INITIAL_MEMORY "268435456")
endif()

if(NOT EMSCRIPTEN)
    target_link_libraries(aowis-server-gui
        PRIVATE
            Qt6::Positioning
            Qt6::SerialPort
    )

    target_sources(aowis-server-gui
        PRIVATE
            src/gps/gps_provider.h
            src/gps/gps_provider.cpp
    )

    target_compile_definitions(aowis-server-gui
        PRIVATE
            AOWIS_HAS_SERIAL_GPS=1
    )
else()
    target_sources(aowis-server-gui
        PRIVATE
            src/gps/gps_provider_dummy.h
    )
    target_compile_definitions(aowis-server-gui
        PRIVATE
            AOWIS_HAS_SERIAL_GPS=0
    )
endif()

if(BUILD_TESTING AND NOT EMSCRIPTEN)
    # Run the actual linked application in a headless, opt-in acceptance mode.
    # This exercises production EntityInspectorTank and HydraulicData rather
    # than a second test-only implementation or an isolated text helper.
    target_sources(aowis-server-gui PRIVATE
        tests/entity_inspector_live_language_test.cpp)
    target_compile_definitions(aowis-server-gui PRIVATE
        AOWIS_GUI_LOCALIZATION_ACCEPTANCE=1)
    add_test(NAME aowis-entity-inspector-live-language
        COMMAND $<TARGET_FILE:aowis-server-gui> --test-live-entity-inspector-language)
    set_tests_properties(aowis-entity-inspector-live-language PROPERTIES
        LABELS "aowis;acceptance" ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30)

    # Link the renderer from an executable that contains NO GUI application
    # sources. A static library's Qt resources need explicit registration;
    # this test fails if resource objects aren't brought in by the linker.
    add_executable(aowis-render-core-link-tests
        tests/render_core_link_test.cpp)
    target_link_libraries(aowis-render-core-link-tests PRIVATE AOWIS::RenderCore)
    target_compile_definitions(aowis-render-core-link-tests PRIVATE
        AOWIS_RENDER_TEST_SHADER_PATH=":/aowis/map/bgfx/${AOWIS_BGFX_TEST_SHADER_FAMILY}/vs_network_2d.bin")
    add_test(NAME aowis-render-core-link COMMAND aowis-render-core-link-tests)
    set_tests_properties(aowis-render-core-link PROPERTIES LABELS "aowis")

    # The four-mode contract is independent of Qt and the graphics backend.
    add_executable(aowis-map-workspace-contract-tests
        tests/map_workspace_contract_test.cpp)
    target_include_directories(aowis-map-workspace-contract-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-map-workspace-contract
        COMMAND aowis-map-workspace-contract-tests)
    set_tests_properties(aowis-map-workspace-contract PROPERTIES LABELS "aowis")

    # GPU-free ownership arbitration regression; isolated from the production
    # renderer target to retain injectable bgfx test functions.
    # Fake init/shutdown callbacks test lifecycle without a GPU or native window.
    add_executable(aowis-bgfx-device-manager-tests
        tests/bgfx_device_manager_test.cpp)
    target_include_directories(aowis-bgfx-device-manager-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-device-manager-tests PRIVATE bgfx AOWIS::RenderDevice)
    add_test(NAME aowis-bgfx-device-manager COMMAND aowis-bgfx-device-manager-tests)
    set_tests_properties(aowis-bgfx-device-manager PROPERTIES LABELS "aowis")

    # Step 2: pure C++ view-ID and global frame arbitration tests. No GPU or
    # native window required (frame API injected).
    add_executable(aowis-bgfx-frame-coordinator-tests
        tests/bgfx_frame_coordinator_test.cpp)
    target_link_libraries(aowis-bgfx-frame-coordinator-tests PRIVATE AOWIS::RenderDevice)
    target_include_directories(aowis-bgfx-frame-coordinator-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-bgfx-frame-coordinator COMMAND aowis-bgfx-frame-coordinator-tests)
    set_tests_properties(aowis-bgfx-frame-coordinator PROPERTIES LABELS "aowis")

    # Step 2B: native swapchain target lifetimes and resize behavior.
    add_executable(aowis-bgfx-native-target-registry-tests
        tests/bgfx_native_target_registry_test.cpp)
    target_include_directories(aowis-bgfx-native-target-registry-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-native-target-registry-tests PRIVATE bgfx AOWIS::RenderDevice)
    add_test(NAME aowis-bgfx-native-target-registry
        COMMAND aowis-bgfx-native-target-registry-tests)
    set_tests_properties(aowis-bgfx-native-target-registry PROPERTIES LABELS "aowis")

    # Step 2C: verify two views render into independent bound targets before
    # one process-wide frame; secondary resizing/retirement never stops map.
    add_executable(aowis-bgfx-two-view-coordination-tests
        tests/bgfx_two_view_coordination_test.cpp)
    target_include_directories(aowis-bgfx-two-view-coordination-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-two-view-coordination-tests PRIVATE bgfx AOWIS::RenderDevice)
    add_test(NAME aowis-bgfx-two-view-coordination
        COMMAND aowis-bgfx-two-view-coordination-tests)
    set_tests_properties(aowis-bgfx-two-view-coordination PROPERTIES LABELS "aowis")

    # Step 5: independent Mercator cameras + three target/view lifecycles.
    # No native windows or graphics device are required for this test.
    add_executable(aowis-bgfx-three-view-camera-tests
        tests/bgfx_three_view_camera_test.cpp)
    target_include_directories(aowis-bgfx-three-view-camera-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-three-view-camera-tests PRIVATE bgfx AOWIS::RenderDevice)
    add_test(NAME aowis-bgfx-three-view-camera
        COMMAND aowis-bgfx-three-view-camera-tests)
    set_tests_properties(aowis-bgfx-three-view-camera PROPERTIES LABELS "aowis")

    add_executable(aowis-map-workspace-diagnostics-tests
        tests/map_workspace_diagnostics_test.cpp)
    target_include_directories(aowis-map-workspace-diagnostics-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-workspace-diagnostics-tests PRIVATE Qt6::Core)
    add_test(NAME aowis-map-workspace-diagnostics
        COMMAND aowis-map-workspace-diagnostics-tests)
    set_tests_properties(aowis-map-workspace-diagnostics PROPERTIES LABELS "aowis")

    # Panels are independent QWidget trees; no GPU/native window is needed.
    add_executable(aowis-map-mode-tool-panel-tests
        tests/map_mode_tool_panel_test.cpp
        src/tabs/map_mode_tool_panel.cpp)
    target_include_directories(aowis-map-mode-tool-panel-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-mode-tool-panel-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-map-mode-tool-panel
        COMMAND aowis-map-mode-tool-panel-tests)
    set_tests_properties(aowis-map-mode-tool-panel PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Independent ranked-DAG layout kernel: no Qt or renderer dependencies.
    add_executable(aowis-compound-dag-layout-tests
        tests/compound_dag_layout_test.cpp
        src/graph/compound_dag_layout.h)
    target_include_directories(aowis-compound-dag-layout-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(aowis-compound-dag-layout-tests PRIVATE cxx_std_17)
    add_test(NAME aowis-compound-dag-layout COMMAND aowis-compound-dag-layout-tests)
    set_tests_properties(aowis-compound-dag-layout PROPERTIES LABELS "aowis")

    # Built-in layered DAG: independent C++17 engine, no Graphviz/Qt required.
    add_executable(aowis-layered-dag-layout-tests
        tests/layered_dag_layout_test.cpp
        src/graph/layered_dag_layout.h)
    target_include_directories(aowis-layered-dag-layout-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(aowis-layered-dag-layout-tests PRIVATE cxx_std_17)
    add_test(NAME aowis-layered-dag-layout COMMAND aowis-layered-dag-layout-tests)
    set_tests_properties(aowis-layered-dag-layout PROPERTIES LABELS "aowis")

    # Projects DAG navigation uses a QWidget fallback (no bgfx instance needed).
    add_executable(aowis-project-revision-graph-tests
        tests/project_revision_graph_test.cpp
        src/tabs/project_revision_graph.cpp
        src/tabs/project_revision_graph.h
        src/tabs/project_graph_placement.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp
        src/config/gui_config_store.cpp
        src/config/shortcut_registry.cpp
        src/config/shortcut_registry.h)
    target_include_directories(aowis-project-revision-graph-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-revision-graph-tests PRIVATE Qt6::Widgets aowis::config)
    add_test(NAME aowis-project-revision-graph
        COMMAND aowis-project-revision-graph-tests)
    set_tests_properties(aowis-project-revision-graph PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Qt-independent backend selection policy: native X11/Wayland default to
    # bgfx, while offscreen/unsupported platforms and explicit opt-out retain
    # QPainter. Safe to exercise in CI without any GPU dependencies.
    add_executable(aowis-project-graph-render-policy-tests
        tests/project_graph_render_policy_test.cpp)
    target_include_directories(aowis-project-graph-render-policy-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-project-graph-render-policy
        COMMAND aowis-project-graph-render-policy-tests)
    set_tests_properties(aowis-project-graph-render-policy PROPERTIES LABELS "aowis")

    # Pure CPU graph-to-bgfx draw-list contract. This test compiles the scene
    # and text atlas in isolation; no GPU or native surface is required.
    # This test needs neither a GPU nor a bgfx device initialization.
    add_executable(aowis-project-graph-bgfx-scene-tests
        tests/project_graph_bgfx_scene_test.cpp
        src/tabs/project_graph_bgfx_scene.cpp
        src/tabs/project_graph_bgfx_scene.h)
    target_include_directories(aowis-project-graph-bgfx-scene-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-graph-bgfx-scene-tests PRIVATE Qt6::Widgets AOWIS::RenderCore)
    add_test(NAME aowis-project-graph-bgfx-scene COMMAND aowis-project-graph-bgfx-scene-tests)
    set_tests_properties(aowis-project-graph-bgfx-scene PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Opt-in CPU benchmark for actual Projects DAG layout and bgfx scene
    # staging. Deliberately not a GPU/FPS benchmark. The large 100k case is
    # opt-in (--stress), so normal CTest remains fast and deterministic.
    add_executable(aowis-project-graph-performance-benchmark
        tests/project_graph_performance_benchmark.cpp
        src/tabs/project_graph_bgfx_scene.cpp
        src/tabs/project_graph_bgfx_scene.h
        src/tabs/project_graph_placement.h)
    target_include_directories(aowis-project-graph-performance-benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-graph-performance-benchmark PRIVATE Qt6::Widgets AOWIS::RenderCore)
    add_test(NAME aowis-project-graph-performance-smoke
        COMMAND aowis-project-graph-performance-benchmark --sizes=64 --repeats=1)
    set_tests_properties(aowis-project-graph-performance-smoke PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Deterministic loopback REST contract; no external DVMDB daemon required.
    add_executable(aowis-project-source-reader-tests
        tests/project_source_reader_test.cpp
        src/projects/project_source_reader.cpp
        src/projects/project_source_reader.h
    )
    target_include_directories(aowis-project-source-reader-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-source-reader-tests PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME aowis-project-source-reader COMMAND aowis-project-source-reader-tests)
    set_tests_properties(aowis-project-source-reader PROPERTIES LABELS "aowis")

    # DVMDB write contract: POST project/branch only, with no external daemon.
    add_executable(aowis-project-source-writer-tests
        tests/project_source_writer_test.cpp
        src/projects/repository_probe.cpp
        src/network/wireguard_runtime_status.cpp
        src/network/wireguard_runtime_status.h
        src/projects/project_source_writer.cpp
        src/projects/project_source_writer.h)
    target_include_directories(aowis-project-source-writer-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-source-writer-tests PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME aowis-project-source-writer COMMAND aowis-project-source-writer-tests)
    set_tests_properties(aowis-project-source-writer PROPERTIES LABELS "aowis")

    # Actual GUI writer -> cpp-httplib -> DVMDB roundtrip against the real
    # server executable, including a persisted working-copy and restart.
    # Set AOWIS_DVMDB_SERVER_BIN at ctest runtime; absent server is SKIP, not PASS.
    add_executable(aowis-project-source-writer-live-dvmdb-tests
        tests/project_source_writer_live_dvmdb_test.cpp
        src/projects/repository_probe.cpp
        src/network/wireguard_runtime_status.cpp
        src/network/wireguard_runtime_status.h
        src/projects/project_source_writer.cpp
        src/projects/project_source_writer.h)
    target_include_directories(aowis-project-source-writer-live-dvmdb-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-source-writer-live-dvmdb-tests PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME aowis-project-source-writer-live-dvmdb
        COMMAND aowis-project-source-writer-live-dvmdb-tests)
    set_tests_properties(aowis-project-source-writer-live-dvmdb PROPERTIES
        LABELS "aowis;integration;http;destructive" SKIP_RETURN_CODE 77 TIMEOUT 90)

    # Real TopControlBarContent geometry and editor-state regression through
    # live Qt translator/RTL changes. No GPU or hydraulic server required.
    add_executable(aowis-top-control-bar-language-tests
        tests/top_control_bar_language_test.cpp
        src/app/top_control_bar_content.h
        src/config/gui_language.cpp
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp
        src/config/gui_config_store.cpp
        translations/translations.qrc)
    target_include_directories(aowis-top-control-bar-language-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-top-control-bar-language-tests PRIVATE Qt6::Widgets aowis::config)
    if(TARGET aowis-gui-compiled-translations)
        add_dependencies(aowis-top-control-bar-language-tests aowis-gui-compiled-translations)
    endif()
    add_test(NAME aowis-top-control-bar-language
        COMMAND aowis-top-control-bar-language-tests)
    set_tests_properties(aowis-top-control-bar-language PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)

    add_executable(aowis-gui-language-tests
        tests/gui_language_test.cpp
        src/app/history_dock.cpp
        src/settings/keyboard_shortcuts_settings_widget.cpp
        src/config/shortcut_registry.cpp
        src/widgets/wasm_popup_menu.cpp
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp
        src/config/gui_config_store.cpp
        src/config/gui_language.cpp
        translations/translations.qrc)
    target_include_directories(aowis-gui-language-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-gui-language-tests PRIVATE Qt6::Widgets aowis::config)
    if(TARGET aowis-gui-compiled-translations)
        add_dependencies(aowis-gui-language-tests aowis-gui-compiled-translations)
    endif()
    add_test(NAME aowis-gui-language COMMAND aowis-gui-language-tests)
    set_tests_properties(aowis-gui-language PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # WireGuard VPN settings on the GUI's AOWIS-CONFIG store: CRUD, "Used by"
    # from configured connections, guarded deletion, damaged records. Uses
    # AOWIS-NETWORK's parser where the embedded engine is built (see below).
    add_executable(aowis-wireguard-vpn-settings-tests
        tests/wireguard_vpn_settings_test.cpp
        src/settings/wireguard_vpn_settings_widget.cpp
        src/settings/wireguard_vpn_settings_widget.h
        src/network/wireguard_runtime_status.cpp
        src/network/wireguard_runtime_status.h
        src/config/wireguard_profile_storage.cpp
        src/config/wireguard_profile_storage.h
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h)
    target_include_directories(aowis-wireguard-vpn-settings-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-wireguard-vpn-settings-tests PRIVATE Qt6::Widgets aowis::config)
    add_test(NAME aowis-wireguard-vpn-settings
        COMMAND aowis-wireguard-vpn-settings-tests)
    set_tests_properties(aowis-wireguard-vpn-settings PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Configured DVMDB connections in the GUI's AOWIS-CONFIG store.
    add_executable(aowis-project-server-profile-storage-tests
        tests/project_server_profile_storage_test.cpp
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h)
    target_include_directories(aowis-project-server-profile-storage-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-project-server-profile-storage-tests PRIVATE Qt6::Core aowis::config)
    add_test(NAME aowis-project-server-profile-storage
        COMMAND aowis-project-server-profile-storage-tests)
    set_tests_properties(aowis-project-server-profile-storage PROPERTIES LABELS "aowis")

    # GUI preferences in AOWIS-CONFIG; former INI groups removed.
    add_executable(aowis-gui-preferences-tests
        tests/gui_preferences_test.cpp
        src/config/gui_preferences.cpp
        src/config/gui_preferences.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/config/gui_configuration.cpp)
    target_include_directories(aowis-gui-preferences-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-gui-preferences-tests PRIVATE
        Qt6::Widgets aowis::config)
    add_test(NAME aowis-gui-preferences
        COMMAND aowis-gui-preferences-tests)
    set_tests_properties(aowis-gui-preferences PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Map-server connection in AOWIS-CONFIG; former INI section removed.
    add_executable(aowis-map-server-client-configuration-tests
        tests/map_server_client_configuration_test.cpp
        src/config/map_server_client_configuration.cpp
        src/config/map_server_client_configuration.h
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp)
    target_include_directories(aowis-map-server-client-configuration-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-server-client-configuration-tests PRIVATE
        Qt6::Widgets aowis::config)
    add_test(NAME aowis-map-server-client-configuration
        COMMAND aowis-map-server-client-configuration-tests)
    set_tests_properties(aowis-map-server-client-configuration PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # VC-03: personal repository is selected in the Projects Server Inspector.
    # UUIDs are discovered and pinned automatically; no separate settings page.
    add_executable(aowis-personal-repository-settings-tests
        tests/personal_repository_settings_test.cpp
        src/config/personal_repository_storage.cpp
        src/config/personal_repository_storage.h
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp)
    target_include_directories(aowis-personal-repository-settings-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-personal-repository-settings-tests PRIVATE
        Qt6::Widgets Qt6::Network aowis::config)
    add_test(NAME aowis-personal-repository-settings
        COMMAND aowis-personal-repository-settings-tests)
    set_tests_properties(aowis-personal-repository-settings PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # INT-02: durable branch-scoped Commit intents survive process death.
    add_executable(aowis-personal-commit-intent-store-tests
        tests/personal_commit_intent_store_test.cpp
        src/projects/personal_commit_intent_store.cpp
        src/projects/personal_commit_intent_store.h)
    target_include_directories(aowis-personal-commit-intent-store-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-personal-commit-intent-store-tests PRIVATE Qt6::Core)
    add_test(NAME aowis-personal-commit-intent-store
        COMMAND aowis-personal-commit-intent-store-tests)
    set_tests_properties(aowis-personal-commit-intent-store PROPERTIES LABELS "aowis")

    # VC-04: personal DVMDB draft creation/recovery, never source-profile writes.
    add_executable(aowis-personal-working-copy-client-tests
        tests/personal_working_copy_client_test.cpp
        src/projects/personal_working_copy_client.cpp
        src/projects/personal_commit_intent_store.cpp
        src/projects/personal_commit_intent_store.h
        src/projects/personal_working_copy_client.h
        src/projects/personal_edit_queue.cpp
        src/projects/personal_edit_queue.h
        src/projects/hydraulic_draft_event_adapter.cpp
        src/projects/hydraulic_draft_event_adapter.h
        src/projects/hydraulic_draft_entity_registry.h
        src/projects/personal_draft_edit_stage.cpp
        src/projects/personal_draft_edit_stage.h
        src/projects/personal_history_navigation_intent_store.cpp
        src/projects/personal_history_navigation_intent_store.h
        src/projects/personal_draft_edit_session.cpp
        src/projects/personal_draft_edit_session.h
        src/config/personal_repository_storage.cpp
        src/config/personal_repository_storage.h
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp)
    target_include_directories(aowis-personal-working-copy-client-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-personal-working-copy-client-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network AOWIS::ServerModel
        aowis-hydraulic-test-support aowis::config)
    add_test(NAME aowis-personal-working-copy-client
        COMMAND aowis-personal-working-copy-client-tests)
    set_tests_properties(aowis-personal-working-copy-client PROPERTIES LABELS "aowis")

    # Projects uses a real nested dock: closing it must leave the graph
    # visible and selections must still synchronize while it is hidden.
    add_executable(aowis-projects-inspector-dock-tests
        tests/projects_inspector_dock_test.cpp
        src/projects/repository_probe.cpp
        src/network/wireguard_runtime_status.cpp
        src/network/wireguard_runtime_status.h
        src/tabs/tab_projects_widget.cpp
        src/tabs/tab_projects_widget.h
        src/projects/project_source_reader.cpp
        src/projects/project_source_reader.h
        src/projects/project_source_writer.cpp
        src/projects/project_source_writer.h
        src/projects/personal_working_copy_client.cpp
        src/projects/personal_commit_intent_store.cpp
        src/projects/personal_commit_intent_store.h
        src/projects/personal_working_copy_client.h
        src/projects/personal_edit_queue.cpp
        src/projects/personal_edit_queue.h
        src/tabs/personal_working_copy_dialog.cpp
        src/tabs/personal_working_copy_dialog.h
        src/config/personal_repository_storage.cpp
        src/config/personal_repository_storage.h
        src/tabs/project_revision_graph.cpp
        src/tabs/project_revision_graph.h
        src/tabs/project_graph_placement.h
        src/app/builtin_examples.cpp
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/widgets/group_box_collapsible.cpp
        src/widgets/group_box_collapsible.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp
        src/config/shortcut_registry.cpp
        src/config/shortcut_registry.h)
    target_include_directories(aowis-projects-inspector-dock-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-projects-inspector-dock-tests PRIVATE Qt6::Widgets Qt6::Network AOWIS::ServerModel
        aowis::config)
    add_test(NAME aowis-projects-inspector-dock
        COMMAND aowis-projects-inspector-dock-tests)
    set_tests_properties(aowis-projects-inspector-dock PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30)

    add_executable(aowis-projects-multi-source-tests
        tests/projects_multi_source_test.cpp
        src/projects/repository_probe.cpp
        src/network/wireguard_runtime_status.cpp
        src/network/wireguard_runtime_status.h
        src/tabs/tab_projects_widget.cpp
        src/tabs/tab_projects_widget.h
        src/projects/project_source_reader.cpp
        src/projects/project_source_reader.h
        src/projects/project_source_writer.cpp
        src/projects/project_source_writer.h
        src/projects/personal_working_copy_client.cpp
        src/projects/personal_commit_intent_store.cpp
        src/projects/personal_commit_intent_store.h
        src/projects/personal_working_copy_client.h
        src/projects/personal_edit_queue.cpp
        src/projects/personal_edit_queue.h
        src/tabs/personal_working_copy_dialog.cpp
        src/tabs/personal_working_copy_dialog.h
        src/config/personal_repository_storage.cpp
        src/config/personal_repository_storage.h
        src/tabs/project_revision_graph.cpp
        src/tabs/project_revision_graph.h
        src/tabs/project_graph_placement.h
        src/app/builtin_examples.cpp
        src/config/project_server_profile_storage.cpp
        src/config/project_server_profile_storage.h
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/widgets/group_box_collapsible.cpp
        src/widgets/group_box_collapsible.h
        src/config/gui_configuration.cpp
        src/config/gui_preferences.cpp
        src/config/shortcut_registry.cpp
        src/config/shortcut_registry.h)
    target_include_directories(aowis-projects-multi-source-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-projects-multi-source-tests PRIVATE Qt6::Widgets Qt6::Network AOWIS::ServerModel
        aowis::config)
    add_test(NAME aowis-projects-multi-source COMMAND aowis-projects-multi-source-tests)
    set_tests_properties(aowis-projects-multi-source PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(TARGET AOWIS::NetworkQt AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
        set(catalogue_peer "${CMAKE_CURRENT_BINARY_DIR}/aowis-catalogue-test-peer")
        set(peer_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/wireguard_catalogue_peer.go")
        set(network_bridge "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-NETWORK/bridge")
        add_custom_command(OUTPUT "${catalogue_peer}"
            COMMAND "${CMAKE_COMMAND}" -E env GOWORK=off "${AOWIS_GO_EXECUTABLE}"
                build -mod=readonly -o "${catalogue_peer}" "${peer_source}"
            WORKING_DIRECTORY "${network_bridge}"
            DEPENDS "${peer_source}" "${network_bridge}/go.mod" "${network_bridge}/go.sum"
            VERBATIM)
        add_custom_target(aowis-catalogue-peer-fixture DEPENDS "${catalogue_peer}")
        add_dependencies(aowis-projects-multi-source-tests aowis-catalogue-peer-fixture)
        add_test(NAME aowis-wireguard-catalogue COMMAND aowis-projects-multi-source-tests "${catalogue_peer}")
        set_tests_properties(aowis-wireguard-catalogue PROPERTIES LABELS "aowis;integration"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    endif()

    # Native Windows tab padding must not widen the five-icon map sidebar.
    add_executable(aowis-map-mode-tab-bar-tests
        tests/map_mode_tab_bar_test.cpp)
    target_include_directories(aowis-map-mode-tab-bar-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-mode-tab-bar-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-map-mode-tab-bar
        COMMAND aowis-map-mode-tab-bar-tests)
    set_tests_properties(aowis-map-mode-tab-bar PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The workspace must preserve viewport identity and teardown order.
    add_executable(aowis-map-workspace-viewport-page-tests
        tests/map_workspace_viewport_page_test.cpp
        src/tabs/map_workspace_viewport_page.cpp
        src/tabs/map_mode_tool_panel.cpp)
    target_include_directories(aowis-map-workspace-viewport-page-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-workspace-viewport-page-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-map-workspace-viewport-page
        COMMAND aowis-map-workspace-viewport-page-tests)
    set_tests_properties(aowis-map-workspace-viewport-page PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # An optional Python-only diagnostics log regression test: no Qt or GPU
    # runtime is required, and systems without Python keep the C++ suite.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME aowis-map-workspace-log-checker
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_map_workspace_log_checker.py)
        set_tests_properties(aowis-map-workspace-log-checker PROPERTIES
            LABELS "aowis")
        add_test(NAME aowis-map-globe-editor-performance-report
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_globe_editor_performance_report.py)
        set_tests_properties(aowis-map-globe-editor-performance-report PROPERTIES
            LABELS "aowis")
    endif()

    # Continuous Web Mercator transforms are independent of Qt, bgfx and the
    # legacy integer zoom/cache path. Always test them, even with bgfx off.
    add_executable(aowis-bgfx-2d-projection-tests tests/bgfx_2d_projection_test.cpp)
    target_include_directories(aowis-bgfx-2d-projection-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-2d-projection-tests PRIVATE AOWIS::RenderGeometry)
    add_test(NAME aowis-bgfx-2d-projection COMMAND aowis-bgfx-2d-projection-tests)
    set_tests_properties(aowis-bgfx-2d-projection PROPERTIES LABELS "aowis")

    # 2D imagery tile layout: continuity across imagery levels, coverage,
    # wrap/clamp and ancestor/child fallback. Pure C++, no Qt or bgfx.
    add_executable(aowis-bgfx-2d-imagery-layout-tests tests/bgfx_2d_imagery_layout_test.cpp)
    target_include_directories(aowis-bgfx-2d-imagery-layout-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-bgfx-2d-imagery-layout COMMAND aowis-bgfx-2d-imagery-layout-tests)
    set_tests_properties(aowis-bgfx-2d-imagery-layout PROPERTIES LABELS "aowis")

    # 2D imagery tile atlas: cell placement, gutters, texture coordinates,
    # page sizing for the budget, LRU reuse, never evicting the drawn frame.
    add_executable(aowis-bgfx-2d-imagery-atlas-tests tests/bgfx_2d_imagery_atlas_test.cpp)
    target_include_directories(aowis-bgfx-2d-imagery-atlas-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-bgfx-2d-imagery-atlas-tests PRIVATE Qt6::Core AOWIS::RenderCore)
    add_test(NAME aowis-bgfx-2d-imagery-atlas COMMAND aowis-bgfx-2d-imagery-atlas-tests)
    set_tests_properties(aowis-bgfx-2d-imagery-atlas PROPERTIES LABELS "aowis")

    # The bgfx scene declutters coincident nodes with the shared routine.
    add_executable(aowis-bgfx-2d-scene-tests tests/bgfx_2d_scene_test.cpp)
    target_include_directories(aowis-bgfx-2d-scene-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-bgfx-2d-scene-tests PRIVATE Qt6::Core AOWIS::ServerModel AOWIS::RenderCore)
    add_test(NAME aowis-bgfx-2d-scene COMMAND aowis-bgfx-2d-scene-tests)
    set_tests_properties(aowis-bgfx-2d-scene PROPERTIES LABELS "aowis")

    # Chunked thick-line mesh: chunking, antimeridian wrap, view culling, and
    # the line quad encoding against a CPU mirror of vs_line_2d.sc.
    add_executable(aowis-bgfx-2d-mesh-tests tests/bgfx_2d_mesh_test.cpp)
    target_include_directories(aowis-bgfx-2d-mesh-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    # Qt6::Gui only for QRgb/QColor in the symbology mapping header.
    target_link_libraries(aowis-bgfx-2d-mesh-tests PRIVATE Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderCore)
    add_test(NAME aowis-bgfx-2d-mesh COMMAND aowis-bgfx-2d-mesh-tests)
    set_tests_properties(aowis-bgfx-2d-mesh PROPERTIES LABELS "aowis")

    # Map editor on bgfx (Phase 2) without a GPU: what the overlay builder
    # draws for each editor state, positioned with the continuous camera.
    add_executable(aowis-bgfx-editor-overlay-tests tests/bgfx_editor_overlay_test.cpp
        src/map/bgfx/map_bgfx_editor_overlay.cpp)
    target_include_directories(aowis-bgfx-editor-overlay-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-bgfx-editor-overlay-tests PRIVATE
        Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderGeo GeographicLib_STATIC AOWIS::RenderCore)
    add_test(NAME aowis-bgfx-editor-overlay COMMAND aowis-bgfx-editor-overlay-tests)
    set_tests_properties(aowis-bgfx-editor-overlay PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # 3D-00A: the same editor helpers must accept one screen/world
    # projection, retaining the 2D hit-box fast path but never applying it
    # to a globe or to a missing projection result.
    add_executable(aowis-map-canvas-projection-tests
        tests/map_canvas_projection_test.cpp)
    target_include_directories(aowis-map-canvas-projection-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(aowis-map-canvas-projection-tests PRIVATE
        Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderGeo)
    add_test(NAME aowis-map-canvas-projection COMMAND aowis-map-canvas-projection-tests)
    set_tests_properties(aowis-map-canvas-projection PROPERTIES LABELS "aowis")

    # 3D-00D1: the same creation state as 2D must hide its floating icon and
    # snapped target when a globe ray misses terrain, then recover cleanly.
    add_executable(aowis-map-canvas-preview-tests
        tests/map_canvas_preview_test.cpp
        src/map/editor/map_canvas_placement.cpp)
    target_include_directories(aowis-map-canvas-preview-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-EPANET/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-EPANET/external/epanet/include)
    target_link_libraries(aowis-map-canvas-preview-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network AOWIS::ServerModel)
    if(NOT EMSCRIPTEN)
        target_link_libraries(aowis-map-canvas-preview-tests PRIVATE Qt6::Positioning)
    endif()
    add_test(NAME aowis-map-canvas-preview COMMAND aowis-map-canvas-preview-tests)
    set_tests_properties(aowis-map-canvas-preview PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Stage 9: 100k-anchor ECEF spatial visibility benchmark and regression.
    # Uses only the standard library, so it also compiles in standalone CI.
    add_executable(aowis-map-bgfx-globe-icon-index-tests
        tests/map_bgfx_globe_icon_index_test.cpp)
    target_include_directories(aowis-map-bgfx-globe-icon-index-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-bgfx-globe-icon-index-tests PRIVATE AOWIS::RenderGeometry)
    add_test(NAME aowis-map-bgfx-globe-icon-index
        COMMAND aowis-map-bgfx-globe-icon-index-tests)
    set_tests_properties(aowis-map-bgfx-globe-icon-index PROPERTIES
        LABELS "aowis;performance")

    # Stage 11: cache terrain heights by drawn surface revision, never by camera.
    add_executable(aowis-map-bgfx-globe-height-cache-tests
        tests/map_bgfx_globe_height_cache_test.cpp)
    target_include_directories(aowis-map-bgfx-globe-height-cache-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-bgfx-globe-height-cache-tests PRIVATE AOWIS::RenderGeometry)
    add_test(NAME aowis-map-bgfx-globe-height-cache
        COMMAND aowis-map-bgfx-globe-height-cache-tests)
    set_tests_properties(aowis-map-bgfx-globe-height-cache PROPERTIES
        LABELS "aowis;performance")

    # Stage 10: synthetic 10k/50k/100k Editor icon projection/ordering
    # workloads, measured separately from actual GUI/GPU/DEM frame timings.
    # The --smoke CTest uses tiny workloads and correctness checks only;
    # run the executable without arguments for the full benchmark suite.
    add_executable(aowis-map-bgfx-globe-icon-benchmark
        tests/map_bgfx_globe_icon_benchmark.cpp)
    target_include_directories(aowis-map-bgfx-globe-icon-benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-bgfx-globe-icon-benchmark PRIVATE AOWIS::RenderGeometry)
    add_test(NAME aowis-map-bgfx-globe-icon-benchmark-smoke
        COMMAND aowis-map-bgfx-globe-icon-benchmark --smoke)
    set_tests_properties(aowis-map-bgfx-globe-icon-benchmark-smoke PROPERTIES
        LABELS "aowis;performance")

    # The Model owns the unit-conversion regression test, but not every Model
    # revision or build configuration registers it. Query the Model directory:
    # if(TEST ...) only sees tests registered in the calling directory.
    get_property(aowis_model_registered_tests
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
        PROPERTY TESTS)
    if("aowis-model-unit-conversion" IN_LIST aowis_model_registered_tests)
        # Cross-directory test properties require CMake 3.28; older toolchains
        # still register and run the Model test without the GUI suite label.
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
            set_tests_properties(aowis-model-unit-conversion
                DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
                PROPERTIES LABELS "aowis")
        else()
            message(STATUS
                "Model unit-conversion test registered; skipping its cross-directory "
                "GUI suite label (requires CMake 3.28 or newer)")
        endif()
    else()
        message(STATUS "Model unit-conversion test is not registered; skipping its GUI suite label")
    endif()

    # Dimension-aware ordering for the mixed-unit valve-setting table columns.
    add_executable(aowis-valve-setting-sort-policy-tests
        tests/valve_setting_sort_policy_test.cpp)
    target_include_directories(aowis-valve-setting-sort-policy-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME aowis-valve-setting-sort-policy COMMAND aowis-valve-setting-sort-policy-tests)
    set_tests_properties(aowis-valve-setting-sort-policy PROPERTIES LABELS "aowis")

    # Intentionally put the old GUI include root first: even that path must
    # resolve to the Model's authoritative unit conversion registry.
    add_executable(aowis-unit-model-header-authority-tests tests/unit_model_header_authority_test.cpp)
    target_include_directories(aowis-unit-model-header-authority-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    add_test(NAME aowis-unit-model-header-authority COMMAND aowis-unit-model-header-authority-tests)
    set_tests_properties(aowis-unit-model-header-authority PROPERTIES LABELS "aowis")

    find_program(AOWIS_NODE_EXECUTABLE NAMES node nodejs)
    if(AOWIS_NODE_EXECUTABLE)
        add_test(NAME aowis-bgfx-map-canvas
            COMMAND ${AOWIS_NODE_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bgfx_map_canvas_test.js)
        set_tests_properties(aowis-bgfx-map-canvas PROPERTIES LABELS "aowis")
    endif()
    add_executable(aowis-emitter-coefficient-binding-tests tests/emitter_coefficient_binding_test.cpp)
    target_include_directories(aowis-emitter-coefficient-binding-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    add_test(NAME aowis-emitter-coefficient-binding COMMAND aowis-emitter-coefficient-binding-tests)
    set_tests_properties(aowis-emitter-coefficient-binding PROPERTIES LABELS "aowis")

    add_executable(aowis-emitter-numeric-spin-box-tests tests/emitter_numeric_spin_box_test.cpp)
    target_include_directories(aowis-emitter-numeric-spin-box-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-emitter-numeric-spin-box-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-emitter-numeric-spin-box COMMAND aowis-emitter-numeric-spin-box-tests)
    set_tests_properties(aowis-emitter-numeric-spin-box PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(aowis-coordinate-property-binding-tests tests/coordinate_property_binding_test.cpp)
    target_include_directories(aowis-coordinate-property-binding-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    add_test(NAME aowis-coordinate-property-binding COMMAND aowis-coordinate-property-binding-tests)
    set_tests_properties(aowis-coordinate-property-binding PROPERTIES LABELS "aowis")

    add_executable(aowis-coordinate-numeric-spin-box-tests tests/coordinate_numeric_spin_box_test.cpp)
    target_include_directories(aowis-coordinate-numeric-spin-box-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-coordinate-numeric-spin-box-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-coordinate-numeric-spin-box COMMAND aowis-coordinate-numeric-spin-box-tests)
    set_tests_properties(aowis-coordinate-numeric-spin-box PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(aowis-unit-numeric-spin-box-tests tests/unit_numeric_spin_box_test.cpp)
    target_include_directories(aowis-unit-numeric-spin-box-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-unit-numeric-spin-box-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-unit-numeric-spin-box COMMAND aowis-unit-numeric-spin-box-tests)
    set_tests_properties(aowis-unit-numeric-spin-box PROPERTIES LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(aowis-unit-table-presentation-tests
        tests/unit_table_presentation_test.cpp
        translations/translations.qrc)
    target_include_directories(aowis-unit-table-presentation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-unit-table-presentation-tests PRIVATE Qt6::Core)
    add_test(NAME aowis-unit-table-presentation COMMAND aowis-unit-table-presentation-tests)
    set_tests_properties(aowis-unit-table-presentation PROPERTIES LABELS "aowis")

    add_executable(aowis-numeric-text-tests tests/numeric_text_test.cpp)
    target_include_directories(aowis-numeric-text-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-numeric-text-tests PRIVATE Qt6::Core)
    add_test(NAME aowis-numeric-text COMMAND aowis-numeric-text-tests)
    set_tests_properties(aowis-numeric-text PROPERTIES LABELS "aowis")

    add_executable(aowis-numeric-editor-adapters-tests
        tests/numeric_editor_adapters_test.cpp
        tests/slider_number_control_test.cpp
        src/widgets/slider_number_control.cpp
        src/widgets/slider_number_control.h
    )
    target_include_directories(aowis-numeric-editor-adapters-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-numeric-editor-adapters-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-numeric-editor-adapters COMMAND aowis-numeric-editor-adapters-tests)
    set_tests_properties(aowis-numeric-editor-adapters PROPERTIES LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(aowis-numeric-property-binding-tests tests/numeric_property_binding_test.cpp)
    target_include_directories(aowis-numeric-property-binding-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-numeric-property-binding COMMAND aowis-numeric-property-binding-tests)
    set_tests_properties(aowis-numeric-property-binding PROPERTIES LABELS "aowis")
    add_executable(aowis-unit-profile-storage-tests
        tests/unit_profile_storage_test.cpp
        src/config/unit_profile_storage.cpp
        src/config/gui_config_store.cpp
        src/config/gui_config_store.h
        src/config/gui_preferences.cpp
        src/config/gui_preferences.h
    )
    target_include_directories(aowis-unit-profile-storage-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include
    )
    target_link_libraries(aowis-unit-profile-storage-tests PRIVATE Qt6::Core aowis::config)
    add_test(NAME aowis-unit-profile-storage COMMAND aowis-unit-profile-storage-tests)
    set_tests_properties(aowis-unit-profile-storage PROPERTIES LABELS "aowis")
    # Regular CTest regression: assert structural performance invariants without
    # imposing machine-dependent timing limits. Print measured timings as JSON.
    add_executable(aowis-unit-presentation-benchmark
        tests/unit_presentation_performance_benchmark.cpp)
    target_include_directories(aowis-unit-presentation-benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-unit-presentation-benchmark PRIVATE Qt6::Widgets)
    add_test(NAME aowis-unit-presentation-benchmark
        COMMAND aowis-unit-presentation-benchmark)
    set_tests_properties(aowis-unit-presentation-benchmark PROPERTIES
        LABELS "aowis;performance"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Group benchmark executables independently of the full regression suite.
    # Add future performance targets here and label their CTest tests "performance".
    add_custom_target(aowis-benchmarks
        DEPENDS
            aowis-unit-presentation-benchmark
            aowis-map-bgfx-globe-icon-index-tests
            aowis-map-bgfx-globe-height-cache-tests
            aowis-map-bgfx-globe-icon-benchmark)

    # Step 2: camera changes and imagery LOD do not invalidate the retained
    # network scene. These are fast deterministic policy checks, not timings.
    # Upload burst policy is a pure C++ contract; no Qt/GPU surface needed.
    add_executable(aowis-map-tile-streaming-policy-tests
        tests/map_tile_streaming_policy_test.cpp)
    target_include_directories(aowis-map-tile-streaming-policy-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-map-tile-streaming-policy
        COMMAND aowis-map-tile-streaming-policy-tests)
    set_tests_properties(aowis-map-tile-streaming-policy PROPERTIES LABELS "aowis;performance")

    add_executable(aowis-map-camera-invalidation-tests
        tests/map_camera_invalidation_test.cpp)
    target_include_directories(aowis-map-camera-invalidation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-map-camera-invalidation-tests PRIVATE Qt6::Core)
    add_test(NAME aowis-map-camera-invalidation
        COMMAND aowis-map-camera-invalidation-tests)
    set_tests_properties(aowis-map-camera-invalidation PROPERTIES LABELS "aowis")

    # Continuous camera interpolation is independent of discrete imagery LOD.
    add_executable(aowis-map-zoom-animation-tests
        tests/map_zoom_animation_test.cpp)
    target_include_directories(aowis-map-zoom-animation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME aowis-map-zoom-animation
        COMMAND aowis-map-zoom-animation-tests)
    set_tests_properties(aowis-map-zoom-animation PROPERTIES LABELS "aowis")

    # Step 3: backend-neutral snapshot retention, UUID identity and coarse
    # geographic chunk metadata. No graphics backend or GPU is required.
    add_executable(aowis-map-retained-network-scene-tests
        tests/map_retained_network_scene_test.cpp)
    target_include_directories(aowis-map-retained-network-scene-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-map-retained-network-scene-tests PRIVATE
        Qt6::Core AOWIS::ServerModel)
    add_test(NAME aowis-map-retained-network-scene
        COMMAND aowis-map-retained-network-scene-tests)
    set_tests_properties(aowis-map-retained-network-scene PROPERTIES LABELS "aowis")

    # Step 4: actual Globe scene remains anchored to its geometry across
    # camera/terrain ECEF origin rebases, including 10,000 demand points.
    add_executable(aowis-map-globe-retained-origin-tests
        tests/map_globe_retained_origin_test.cpp)
    target_include_directories(aowis-map-globe-retained-origin-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-map-globe-retained-origin-tests PRIVATE
        Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderGeo AOWIS::RenderGlobeNetwork GeographicLib_STATIC AOWIS::RenderCore AOWIS::RenderGlobe)
    add_test(NAME aowis-map-globe-retained-origin
        COMMAND aowis-map-globe-retained-origin-tests)
    set_tests_properties(aowis-map-globe-retained-origin PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Camera-depth sorting of alpha-blended globe icons; no GPU context needed.
    add_executable(aowis-map-globe-icon-draw-order-tests
        tests/map_globe_icon_draw_order_test.cpp)
    target_include_directories(aowis-map-globe-icon-draw-order-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-map-globe-icon-draw-order-tests PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME aowis-map-globe-icon-draw-order
        COMMAND aowis-map-globe-icon-draw-order-tests)
    set_tests_properties(aowis-map-globe-icon-draw-order PROPERTIES LABELS "aowis")

    # Demand points (no elevation in the model) lie on the drawn globe
    # terrain: bare ellipsoid, relief, in-place moves without a network
    # rebuild, attachment lines ending at the network's elevation.
    add_executable(aowis-map-globe-demand-drape-tests
        tests/map_globe_demand_drape_test.cpp)
    target_include_directories(aowis-map-globe-demand-drape-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-map-globe-demand-drape-tests PRIVATE
        Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderGeo AOWIS::RenderGlobeNetwork GeographicLib_STATIC AOWIS::RenderCore AOWIS::RenderGlobe)
    add_test(NAME aowis-map-globe-demand-drape
        COMMAND aowis-map-globe-demand-drape-tests)
    set_tests_properties(aowis-map-globe-demand-drape PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Intermediate pipe bends follow the drawn globe terrain without changing
    # hydraulic endpoint elevations or rebuilding on unchanged terrain samples.
    add_executable(aowis-map-globe-pipe-bend-drape-tests
        tests/map_globe_pipe_bend_drape_test.cpp)
    target_include_directories(aowis-map-globe-pipe-bend-drape-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-map-globe-pipe-bend-drape-tests PRIVATE
        Qt6::Core Qt6::Gui AOWIS::ServerModel AOWIS::RenderGeo AOWIS::RenderGlobeNetwork GeographicLib_STATIC AOWIS::RenderCore AOWIS::RenderGlobe)
    add_test(NAME aowis-map-globe-pipe-bend-drape
        COMMAND aowis-map-globe-pipe-bend-drape-tests)
    set_tests_properties(aowis-map-globe-pipe-bend-drape PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # End-to-end Qt presentation binding across an editor, a virtualized table,
    # map HUD/legend formatters and simulation readouts. This is headless
    # cross-surface coverage, not a GPU/solver production renderer benchmark.
    add_executable(aowis-unit-cross-surface-acceptance-tests
        tests/unit_cross_surface_acceptance_test.cpp)
    target_include_directories(aowis-unit-cross-surface-acceptance-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include)
    target_link_libraries(aowis-unit-cross-surface-acceptance-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-unit-cross-surface-acceptance
        COMMAND aowis-unit-cross-surface-acceptance-tests)
    set_tests_properties(aowis-unit-cross-surface-acceptance PROPERTIES
        LABELS "aowis;acceptance" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(aowis-unit-aware-view-tests
        tests/unit_aware_view_test.cpp
    )
    target_include_directories(aowis-unit-aware-view-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL/include
    )
    target_link_libraries(aowis-unit-aware-view-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-unit-aware-view COMMAND aowis-unit-aware-view-tests)
    set_tests_properties(aowis-unit-aware-view PROPERTIES
        LABELS "aowis"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_executable(aowis-map-render-neutral-tests
        tests/map_render_neutral_tests.cpp
        tests/test_harness.h
    )
    target_include_directories(aowis-map-render-neutral-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(aowis-map-render-neutral-tests
        PRIVATE
            Qt6::Core
            Qt6::Gui
            AOWIS::ServerModel
            AOWIS::RenderGeo
            AOWIS::RenderTerrainData
            AOWIS::RenderView
            AOWIS::RenderGlobeTerrain
            GeographicLib_STATIC
            AOWIS::RenderCore
            AOWIS::RenderGlobe)
    add_test(
        NAME aowis-map-render-neutral
        COMMAND aowis-map-render-neutral-tests
    )
    set_tests_properties(aowis-map-render-neutral
        PROPERTIES
            LABELS "aowis"
    )

    add_library(aowis-hydraulic-test-support STATIC
        src/network/hydraulic_edit_transaction_capture.cpp
        src/network/hydraulic_edit_transaction_capture.h
        src/network/hydraulic_network_editor.cpp
        src/network/hydraulic_network_editor.h
        src/network/network_render_snapshot_builder.cpp
        src/network/network_render_snapshot_builder.h
    )
    target_include_directories(aowis-hydraulic-test-support
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(aowis-hydraulic-test-support
        PUBLIC
            Qt6::Core
            AOWIS::ServerModel
            AOWIS::RenderGeo
            GeographicLib_STATIC
    )

    function(aowis_add_hydraulic_test target_name test_name source_file)
        add_executable(${target_name}
            ${source_file}
            tests/test_harness.h
        )
        target_link_libraries(${target_name}
            PRIVATE
                aowis-hydraulic-test-support
        )
        add_test(
            NAME ${test_name}
            COMMAND ${target_name}
        )
        set_tests_properties(${test_name}
            PROPERTIES
                LABELS "aowis"
        )
    endfunction()

    aowis_add_hydraulic_test(
        aowis-hydraulic-network-editor-tests
        aowis-hydraulic-network-editor
        tests/hydraulic_network_editor_tests.cpp
    )
    add_executable(aowis-personal-edit-queue-tests
        tests/personal_edit_queue_test.cpp
        tests/test_harness.h
        src/projects/personal_edit_queue.cpp
        src/projects/personal_edit_queue.h
    )
    target_include_directories(aowis-personal-edit-queue-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(aowis-personal-edit-queue-tests PRIVATE
        Qt6::Core AOWIS::ServerModel)
    add_test(NAME aowis-personal-edit-queue COMMAND aowis-personal-edit-queue-tests)
    set_tests_properties(aowis-personal-edit-queue PROPERTIES LABELS "aowis")

    add_executable(aowis-history-dock-tests
        tests/history_dock_test.cpp
        src/app/history_dock.cpp
        src/app/history_dock.h
    )
    target_include_directories(aowis-history-dock-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-history-dock-tests PRIVATE Qt6::Widgets)
    add_test(NAME aowis-history-dock COMMAND aowis-history-dock-tests)
    set_tests_properties(aowis-history-dock PROPERTIES LABELS "aowis")

    add_executable(aowis-hydraulic-edit-transaction-tests
        tests/hydraulic_edit_transaction_test.cpp
        tests/test_harness.h
        src/network/hydraulic_data.cpp
        src/network/hydraulic_data.h
        src/projects/personal_edit_queue.cpp
        src/projects/personal_edit_queue.h
        src/projects/hydraulic_draft_event_adapter.cpp
        src/projects/hydraulic_draft_event_adapter.h
        src/projects/hydraulic_draft_entity_registry.h
        src/projects/personal_draft_edit_stage.cpp
        src/projects/personal_draft_edit_stage.h
    )
    set_target_properties(aowis-hydraulic-edit-transaction-tests PROPERTIES AUTOMOC ON)
    target_link_libraries(aowis-hydraulic-edit-transaction-tests PRIVATE
        aowis-hydraulic-test-support AOWIS::ServerEpanet Qt6::Core Qt6::Gui Qt6::Network)
    add_test(NAME aowis-hydraulic-edit-transaction
        COMMAND aowis-hydraulic-edit-transaction-tests)
    set_tests_properties(aowis-hydraulic-edit-transaction PROPERTIES LABELS "aowis")

    add_executable(aowis-hydraulic-libraries-widget-tests
        tests/hydraulic_libraries_widget_test.cpp
        src/tabs/tab_hydraulic_libraries_widget.cpp
        src/tabs/tab_hydraulic_libraries_widget.h
        src/network/hydraulic_data.cpp
        src/network/hydraulic_data.h
    )
    target_include_directories(aowis-hydraulic-libraries-widget-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-hydraulic-libraries-widget-tests PRIVATE
        aowis-hydraulic-test-support AOWIS::ServerEpanet
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
    add_test(NAME aowis-hydraulic-libraries-widget
        COMMAND aowis-hydraulic-libraries-widget-tests)
    set_tests_properties(aowis-hydraulic-libraries-widget PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)

    add_executable(aowis-hydraulic-controls-widget-tests
        tests/hydraulic_controls_widget_test.cpp
        src/tabs/tab_hydraulic_controls_widget.cpp
        src/tabs/tab_hydraulic_controls_widget.h
        src/network/hydraulic_data.cpp
        src/network/hydraulic_data.h
        # Widget replay regression calls prepareWithMembership directly.
        src/projects/hydraulic_draft_event_adapter.cpp
        src/projects/hydraulic_draft_event_adapter.h
        src/projects/hydraulic_draft_entity_registry.h
    )
    target_include_directories(aowis-hydraulic-controls-widget-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(aowis-hydraulic-controls-widget-tests PRIVATE
        aowis-hydraulic-test-support AOWIS::ServerEpanet
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
    add_test(NAME aowis-hydraulic-controls-widget
        COMMAND aowis-hydraulic-controls-widget-tests)
    set_tests_properties(aowis-hydraulic-controls-widget PROPERTIES
        LABELS "aowis" ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)

    aowis_add_hydraulic_test(
        aowis-demand-point-tests
        aowis-demand-points
        tests/demand_point_tests.cpp
    )
    aowis_add_hydraulic_test(
        aowis-pipe-material-tests
        aowis-pipe-materials
        tests/pipe_material_tests.cpp
    )

    add_executable(aowis-epanet-js-reader-tests
        tests/epanet_js_project_reader_tests.cpp
        tests/test_harness.h
    )
    target_include_directories(aowis-epanet-js-reader-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(aowis-epanet-js-reader-tests
        PRIVATE
            aowis-epanet-js-import
            Qt6::Core
            Qt6::Sql
    )
    add_test(
        NAME aowis-epanet-js-reader
        COMMAND aowis-epanet-js-reader-tests
    )
    set_tests_properties(aowis-epanet-js-reader
        PROPERTIES
            LABELS "aowis"
    )

    add_executable(aowis-epanet-js-converter-tests
        tests/epanet_js_project_converter_tests.cpp
        tests/test_harness.h
    )
    target_include_directories(aowis-epanet-js-converter-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(aowis-epanet-js-converter-tests
        PRIVATE
            aowis-epanet-js-import
            Qt6::Core
            AOWIS::ServerModel
    )
    add_test(
        NAME aowis-epanet-js-converter
        COMMAND aowis-epanet-js-converter-tests
    )
    set_tests_properties(aowis-epanet-js-converter
        PROPERTIES
            LABELS "aowis"
    )

    add_executable(aowis-epanet-js-integration-tests
        tests/epanet_js_project_integration_tests.cpp
        tests/test_harness.h
    )
    target_include_directories(aowis-epanet-js-integration-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(aowis-epanet-js-integration-tests
        PRIVATE
            AOWIS_EPANET_JS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/epanet-js"
    )
    target_link_libraries(aowis-epanet-js-integration-tests
        PRIVATE
            aowis-epanet-js-import
            Qt6::Core
            Qt6::Sql
            AOWIS::ServerModel
            AOWIS::ServerEpanet
    )
    add_test(
        NAME aowis-epanet-js-integration
        COMMAND aowis-epanet-js-integration-tests
    )
    set_tests_properties(aowis-epanet-js-integration
        PROPERTIES
            LABELS "aowis"
    )

    if(TARGET AOWIS::NetworkQt)
        add_executable(aowis-repository-probe-tests
            tests/repository_probe_test.cpp src/projects/repository_probe.cpp
            src/network/wireguard_runtime_status.cpp src/network/wireguard_runtime_status.h
            src/config/gui_config_store.cpp src/config/gui_config_store.h
            src/config/wireguard_profile_storage.cpp src/config/wireguard_profile_storage.h)
        target_include_directories(aowis-repository-probe-tests PRIVATE src)
        target_link_libraries(aowis-repository-probe-tests PRIVATE aowis::config)
        add_test(NAME aowis-repository-probe COMMAND aowis-repository-probe-tests)
        set_tests_properties(aowis-repository-probe PROPERTIES LABELS "aowis" TIMEOUT 20)
    endif()

    # Build all locally declared AOWIS test executables, not a manually
    # maintained list. A stale list let Ninja report success while CTest then
    # complained about unbuilt test executables (many pages of "Not Run").
    # Test executables follow the -tests or -benchmark naming convention.
    # Test scripts (Python/Bash) do not require a compiled target here.
    get_property(_aowis_local_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    set(_aowis_test_executables)
    foreach(_aowis_candidate IN LISTS _aowis_local_targets)
        if(_aowis_candidate MATCHES "^aowis-.*-(tests|benchmark)$")
            get_target_property(_aowis_candidate_type
                "${_aowis_candidate}" TYPE)
            if(_aowis_candidate_type STREQUAL "EXECUTABLE")
                list(APPEND _aowis_test_executables "${_aowis_candidate}")
            endif()
        endif()
    endforeach()
    if(NOT _aowis_test_executables)
        message(FATAL_ERROR "No AOWIS test executable targets were registered")
    endif()
    # A green CTest report is meaningful only after the complete GUI also
    # compiles and links. The application and tests share the extracted Render
    # libraries; a test-only build previously passed with a broken GUI link.
    add_custom_target(aowis-tests DEPENDS ${_aowis_test_executables})
    add_dependencies(aowis-tests aowis-server-gui)
    if(TARGET aowis-model-unit-conversion-test)
        add_dependencies(aowis-tests aowis-model-unit-conversion-test)
    endif()
endif()

qt_import_plugins(aowis-server-gui
    INCLUDE_BY_TYPE imageformats
)

if(TARGET AOWIS::NetworkQt AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    foreach(probe_target IN ITEMS aowis-server-gui aowis-repository-probe-tests aowis-projects-inspector-dock-tests aowis-projects-multi-source-tests aowis-wireguard-vpn-settings-tests)
        if(TARGET ${probe_target})
            target_compile_definitions(${probe_target} PRIVATE AOWIS_GUI_WIREGUARD_PROBE)
            target_link_libraries(${probe_target} PRIVATE AOWIS::NetworkQt Qt6::Concurrent)
        endif()
    endforeach()
    # WireGuard profiles are read from AOWIS-CONFIG wherever tunnels start or
    # profiles are chosen; the GUI executable lists the source itself.
    foreach(probe_target IN ITEMS aowis-projects-inspector-dock-tests aowis-projects-multi-source-tests)
        if(TARGET ${probe_target})
            target_sources(${probe_target} PRIVATE
                src/config/wireguard_profile_storage.cpp src/config/wireguard_profile_storage.h)
        endif()
    endforeach()
endif()

if(EMSCRIPTEN AND NOT TARGET Qt6::QSQLiteDriverPlugin)
    message(FATAL_ERROR
        "The WebAssembly build requires Qt6::QSQLiteDriverPlugin so epanet-js "
        ".ejsdb projects can be imported. Rebuild the AOWIS WASM toolchain image."
    )
endif()

if(TARGET Qt6::QSQLiteDriverPlugin)
    qt_import_plugins(aowis-server-gui
        INCLUDE Qt6::QSQLiteDriverPlugin
    )
endif()

if(TARGET aowis-gui-compiled-translations)
    add_dependencies(aowis-server-gui aowis-gui-compiled-translations)
endif()
