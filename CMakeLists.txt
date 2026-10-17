cmake_minimum_required(VERSION 3.24)
project(cs2_skinloader LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Windows-only build.")
endif()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Project-wide defines (so UNICODE/_UNICODE reaches ImGui + wWinMain CRT entry).
add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS
                -DUNICODE -D_UNICODE)

if (MSVC)
    # /utf-8 /EHsc /MP are safe globally. /W4 /permissive- are scoped later
    # to our own targets so FetchContent'd third-party code (ImGui) stays
    # quiet under its own warning expectations.
    add_compile_options(/utf-8 /EHsc /MP)
    # PDBALTPATH only matters when a PDB is produced.
    add_link_options($<$<CONFIG:Debug>:/PDBALTPATH:%_PDB%>)
    add_link_options($<$<CONFIG:RelWithDebInfo>:/PDBALTPATH:%_PDB%>)
endif()

# Helper to apply strict warnings to our own targets only.
function(cheat_strict_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endfunction()

include(FetchContent)

# ---- Dear ImGui ----
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.4
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# ---- nlohmann/json (header-only) ----
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- stb_image (header-only) — pin to a reviewed commit, not master.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG         f0569113c93ad095470c54bf34a17b36646bbbb5
)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(injector)
add_subdirectory(module)
