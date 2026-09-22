cmake_minimum_required(VERSION 3.24)
project(RetopoDirector VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(RD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(RD_LTO               "Enable link time optimisation in release builds" ON)
option(RD_NATIVE_ARCH       "Optimise for the host CPU" OFF)

# ---------------------------------------------------------------------------
# Third party.
# We only let FetchContent *download* the sources; every dependency except GLFW
# is compiled by targets declared right here. That keeps CMake 4.x and the
# MinGW toolchain out of reach of upstream build scripts.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS      OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL         OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.25
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(xatlas
    GIT_REPOSITORY https://github.com/jpcy/xatlas.git
    GIT_TAG        f700c7790aaa030e794b52ba7791a05c085faf0c
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG        v0.9.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR  rd_no_cmake)

FetchContent_MakeAvailable(glfw imgui meshoptimizer xatlas cgltf nlohmann_json stb ufbx)

# --- Dear ImGui ------------------------------------------------------------
add_library(rd_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(rd_imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(rd_imgui PUBLIC glfw)
target_compile_definitions(rd_imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# --- meshoptimizer ---------------------------------------------------------
file(GLOB RD_MESHOPT_SRC ${meshoptimizer_SOURCE_DIR}/src/*.cpp)
add_library(rd_meshoptimizer STATIC ${RD_MESHOPT_SRC})
target_include_directories(rd_meshoptimizer PUBLIC ${meshoptimizer_SOURCE_DIR}/src)

# --- xatlas ----------------------------------------------------------------
add_library(rd_xatlas STATIC ${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp)
target_include_directories(rd_xatlas PUBLIC ${xatlas_SOURCE_DIR}/source/xatlas)
if(NOT MSVC)
    target_compile_options(rd_xatlas PRIVATE
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function)
endif()

# --- ufbx ------------------------------------------------------------------
# One C file, MIT. The Autodesk FBX SDK is the alternative and it cannot be
# redistributed, which rules it out for anything shipped. See docs/LICENSES.md.
add_library(rd_ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(rd_ufbx PUBLIC ${ufbx_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(rd_ufbx PRIVATE -Wno-unused-function -Wno-sign-compare)
endif()
find_package(Threads REQUIRED)
target_link_libraries(rd_ufbx PRIVATE Threads::Threads)
if(NOT WIN32)
    target_link_libraries(rd_ufbx PRIVATE m)
endif()

# --- header-only C deps ----------------------------------------------------
add_library(rd_extern STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/extern_impl.c)
target_include_directories(rd_extern PUBLIC ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(rd_extern PRIVATE -Wno-unused-function -Wno-sign-compare)
endif()

add_library(rd_json INTERFACE)
target_include_directories(rd_json INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RD_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE RD_HEADERS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

add_executable(retopo-director ${RD_SOURCES} ${RD_HEADERS})
target_include_directories(retopo-director PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(retopo-director PRIVATE
    rd_imgui rd_meshoptimizer rd_xatlas rd_extern rd_json rd_ufbx glfw)

find_package(OpenGL REQUIRED)
target_link_libraries(retopo-director PRIVATE OpenGL::GL)

find_package(Threads REQUIRED)
target_link_libraries(retopo-director PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(retopo-director PRIVATE winhttp shlwapi comdlg32 ole32 shell32)
    target_compile_definitions(retopo-director PRIVATE
        RD_PLATFORM_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
elseif(APPLE)
    target_compile_definitions(retopo-director PRIVATE RD_PLATFORM_MACOS=1)
else()
    target_compile_definitions(retopo-director PRIVATE RD_PLATFORM_LINUX=1)
    target_link_libraries(retopo-director PRIVATE ${CMAKE_DL_LIBS})
endif()

target_compile_definitions(retopo-director PRIVATE RD_VERSION_STRING="${PROJECT_VERSION}")

# MinGW links libstdc++, libgcc and winpthread dynamically by default, which
# makes the executable useless outside a shell that has the toolchain on PATH.
# A retopology tool should be a single file you can hand to an artist.
if(MINGW)
    target_link_options(retopo-director PRIVATE
        -static-libgcc -static-libstdc++ -static)
endif()

if(MSVC)
    target_compile_options(retopo-director PRIVATE /W4 /permissive- /MP /utf-8)
    if(RD_WARNINGS_AS_ERRORS)
        target_compile_options(retopo-director PRIVATE /WX)
    endif()
else()
    target_compile_options(retopo-director PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(RD_WARNINGS_AS_ERRORS)
        target_compile_options(retopo-director PRIVATE -Werror)
    endif()
    if(RD_NATIVE_ARCH)
        target_compile_options(retopo-director PRIVATE -march=native)
    endif()
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT RD_IPO_OK OUTPUT RD_IPO_MSG)
if(RD_LTO AND RD_IPO_OK)
    set_property(TARGET retopo-director  PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET rd_meshoptimizer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET rd_ufbx          PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

add_custom_command(TARGET retopo-director POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/profiles $<TARGET_FILE_DIR:retopo-director>/profiles
    COMMENT "Staging target profiles")

# The SAM sidecar. Optional at runtime, but it has to sit next to the binary for
# the segmenter to find it without being told where it is.
add_custom_command(TARGET retopo-director POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/tools $<TARGET_FILE_DIR:retopo-director>/tools
    COMMENT "Staging the segmentation sidecar")

message(STATUS "-------------------------------------------------")
message(STATUS " Retopo Director ${PROJECT_VERSION}")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  LTO      : ${RD_LTO} / supported=${RD_IPO_OK}")
message(STATUS "-------------------------------------------------")
