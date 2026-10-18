option(SK_VAC_RECON "Compile the §6.4 recon utilities" OFF)

set(CHEAT_SOURCES
    dllmain.cpp
    hooks/hook_registry.cpp
    hooks/present_hook.cpp
    hooks/framestage_hook.cpp
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
if (SK_VAC_RECON)
    list(APPEND CHEAT_SOURCES opsec/vac_recon.cpp)
endif()

add_library(cheat SHARED ${CHEAT_SOURCES})
target_link_libraries(cheat PRIVATE imgui nlohmann_json::nlohmann_json stb)
target_include_directories(cheat PRIVATE ${CMAKE_SOURCE_DIR})

set_target_properties(cheat PROPERTIES
    PREFIX ""           # cheat.dll, not libcheat.dll
    OUTPUT_NAME "cheat"
)
cheat_strict_warnings(cheat)
if (SK_VAC_RECON)
    target_compile_definitions(cheat PRIVATE SK_VAC_RECON=1)
endif()
