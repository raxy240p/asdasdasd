add_executable(loader WIN32
    main.cpp
    ui_injector.cpp
    anticheat_gate.cpp
    randomize.cpp
    target_find.cpp
    manual_map.cpp
)
target_link_libraries(loader PRIVATE imgui nlohmann_json::nlohmann_json)
target_include_directories(loader PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(loader PROPERTIES OUTPUT_NAME "loader")
