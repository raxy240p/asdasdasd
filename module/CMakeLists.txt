add_library(cheat SHARED
    dllmain.cpp
    hooks/hook_registry.cpp
    hooks/present_hook.cpp
    hooks/wndproc_hook.cpp
    menu/menu.cpp
    menu/tab_inventory.cpp
    menu/tab_additem.cpp
    menu/tab_settings.cpp
    items/item_db.cpp
    items/preview.cpp
    inventory/backend_phase_a.cpp
    render/dx11_setup.cpp
    render/icon_cache.cpp
    opsec/call_gate.cpp
    opsec/mem_hygiene.cpp
)
target_link_libraries(cheat PRIVATE imgui nlohmann_json::nlohmann_json stb)
target_include_directories(cheat PRIVATE ${CMAKE_SOURCE_DIR})

set_target_properties(cheat PROPERTIES
    PREFIX ""           # cheat.dll, not libcheat.dll
    OUTPUT_NAME "cheat"
)

# Optional recon build (§6.4). Off by default.
option(SK_VAC_RECON    "Compile the §6.4 recon utilities" OFF)
option(SK_MODULE_STOMP "Enable module-stomping mapper fallback (§6.7)" OFF)
if (SK_VAC_RECON)
    target_compile_definitions(cheat PRIVATE SK_VAC_RECON=1)
endif()
if (SK_MODULE_STOMP)
    target_compile_definitions(cheat PRIVATE SK_MODULE_STOMP=1)
endif()
