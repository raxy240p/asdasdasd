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

# ---- stage items.json beside the built loader so first-run finds it ----
# menu::Init() searches configDir, then $SK_ITEMS_DB, then
# <loader-exe-parent>/data/items.json. This copy populates the third fallback
# so a fresh clone can run without hand-staging. Must live in THIS
# CMakeLists.txt — CMake 3.24+ rejects a TARGET custom-command referencing a
# target created in a different directory.
add_custom_command(TARGET loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:loader>/data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/items.json"
            "$<TARGET_FILE_DIR:loader>/data/items.json"
    COMMENT "Stage data/items.json beside loader")
