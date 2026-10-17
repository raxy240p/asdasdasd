option(SK_MODULE_STOMP "Build the module-stomp injection path (§6.7)" OFF)

set(LOADER_SOURCES
    main.cpp
    ui_injector.cpp
    anticheat_gate.cpp
    randomize.cpp
    target_find.cpp
    manual_map.cpp
)
if (SK_MODULE_STOMP)
    list(APPEND LOADER_SOURCES module_stomp.cpp)
endif()

add_executable(loader WIN32 ${LOADER_SOURCES})
target_link_libraries(loader PRIVATE imgui)
target_include_directories(loader PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(loader PROPERTIES OUTPUT_NAME "loader")
cheat_strict_warnings(loader)
if (SK_MODULE_STOMP)
    target_compile_definitions(loader PRIVATE SK_MODULE_STOMP=1)
endif()
