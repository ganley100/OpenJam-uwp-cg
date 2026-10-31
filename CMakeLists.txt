# nbajam_ofe - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nbajam_ofe LANGUAGES CXX)

# RC is only needed to carry the executable's icon (res/app.rc).
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# The codegen tool runs on the build machine, so an Xbox build (whose SDK is a
# UWP build) can be pointed at an ordinary desktop rexglue.exe instead.
set(NBAJAM_CODEGEN_CLI "" CACHE FILEPATH "rexglue.exe to run codegen with, instead of the SDK's own")
if(NBAJAM_CODEGEN_CLI AND TARGET rex::rexglue)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${NBAJAM_CODEGEN_CLI}"
        IMPORTED_LOCATION_RELEASE "${NBAJAM_CODEGEN_CLI}")
endif()

# Sources
option(REXPORT_CRASH_REPORT "Resolve guest call stacks on access violations" ON)
option(REXPORT_GUEST_PROFILER "Sampling profiler for locating guest code" ON)

set(NBAJAMOFE_SOURCES
    src/main.cpp
    src/kernel_usbcam_stubs.cpp
    src/nbajam_hooks.cpp
    src/nbajam_probes.cpp
    src/vp6_hooks.cpp
    src/dds_textures.cpp
    src/scripted_input.cpp
    src/apt_trace.cpp
    src/null_page.cpp
    src/unpack_container.cpp
    src/watchdog.cpp
    src/asset_fallback.cpp
    src/mod_saves.cpp
    src/mod_swap.cpp
    src/mod_picker.cpp
    src/host_pad.cpp
    src/window_title.cpp
)

if(WIN32)
    list(APPEND NBAJAMOFE_SOURCES res/app.rc)
endif()

# Xbox One, as a UWP app in Developer Mode, against a ReXGlue built with the
# patches in xbox/rexglue (REXGLUE_UWP). See XBOX.md and xbox/rexglue.
#
# A UWP app cannot start another process, so the in-game mod swap and the
# watchdog's restart have nothing to work with. src/uwp_platform.cpp stands in
# for both, and each mod is packaged as an app of its own instead.
option(NBAJAM_UWP "Build for the UWP app container (Xbox One Developer Mode)" OFF)

if(NBAJAM_UWP)
    if(NOT WIN32)
        message(FATAL_ERROR "NBAJAM_UWP needs a Windows host")
    endif()
    # Neither runs in an app container: both resolve symbols through DbgHelp,
    # which is a desktop-only library.
    set(REXPORT_CRASH_REPORT OFF CACHE BOOL "" FORCE)
    set(REXPORT_GUEST_PROFILER OFF CACHE BOOL "" FORCE)
    list(REMOVE_ITEM NBAJAMOFE_SOURCES src/mod_swap.cpp src/watchdog.cpp)
    list(APPEND NBAJAMOFE_SOURCES src/uwp_platform.cpp src/uwp_fromapp.cpp src/ps3_mod.cpp
        src/thread_load.cpp)

    # The mod apps open PS3 mods themselves (src/ps3_mod.cpp), whose archives
    # are zlib inside. miniz is zlib in one file.
    enable_language(C)
    include(FetchContent)
    FetchContent_Declare(miniz
        URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
        URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5)
    FetchContent_MakeAvailable(miniz)   # just the sources; it has no CMake of its own
    add_library(nbajam_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
    target_include_directories(nbajam_miniz PUBLIC ${miniz_SOURCE_DIR})
    target_compile_definitions(nbajam_miniz PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME
        MINIZ_NO_ARCHIVE_APIS)
endif()

# iPhone, against a ReXGlue built for iOS with the patches in
# ios/rexglue. As on the Xbox, an app cannot start another process, so
# src/ios_platform.mm stands in for the mod swap and the watchdog, and each
# mod is an app of its own. The controller is drawn on the screen
# (src/ios_touch.cpp). nbajam_hooks.cpp is diagnostics no hook names any
# more, written with Windows' structured exceptions.
option(NBAJAM_IOS "Build for iOS (iPhone)" OFF)

if(NBAJAM_IOS)
    if(NOT IOS)
        message(FATAL_ERROR "NBAJAM_IOS needs CMAKE_SYSTEM_NAME=iOS")
    endif()
    enable_language(OBJCXX)
    list(REMOVE_ITEM NBAJAMOFE_SOURCES src/mod_swap.cpp src/watchdog.cpp src/nbajam_hooks.cpp)
    list(APPEND NBAJAMOFE_SOURCES src/ios_platform.mm src/ios_touch.cpp)
endif()

if(REXPORT_CRASH_REPORT AND WIN32)
    list(APPEND NBAJAMOFE_SOURCES src/crash_report.cpp)
endif()

# Inert unless NBAJAM_PROFILE_SECONDS is set in the environment.
if(REXPORT_GUEST_PROFILER AND WIN32)
    list(APPEND NBAJAMOFE_SOURCES src/guest_profiler.cpp)
endif()

if(WIN32)
    add_executable(nbajam_ofe WIN32 ${NBAJAMOFE_SOURCES})
else()
    add_executable(nbajam_ofe ${NBAJAMOFE_SOURCES})
endif()

# GPU_PLUGINS is required: the Xenos backend is loaded at runtime rather than
# linked, so nothing stages rexgpu-xenos*.dll beside the executable without
# it and the game comes up with no graphics backend. `rexglue init` does not
# emit this.
rexglue_setup_target(nbajam_ofe GPU_PLUGINS xenos)

if(NBAJAM_UWP)
    # Ordinary Windows code, like the patched SDK: only the window and the
    # app's entry are UWP, and the SDK's helper already supplies those, with
    # /APPCONTAINER and WindowsApp.lib. The desktop C++ runtime DLLs ship
    # inside the package (xbox/game/package.ps1).
    target_compile_definitions(nbajam_ofe PRIVATE NBAJAM_UWP=1)
    # A linker map, so a native return address in a log names the recompiled
    # guest function (sub_<address>) it is in.
    target_link_options(nbajam_ofe PRIVATE "LINKER:/MAP")
    # xinputuap.lib is XInput 1.4 as an app links it.
    target_link_libraries(nbajam_ofe PRIVATE xinputuap.lib nbajam_miniz)
endif()

if(NBAJAM_IOS)
    target_compile_definitions(nbajam_ofe PRIVATE NBAJAM_IOS=1)
    # A plain executable: ios/game/package.sh assembles the app around it.
    set_target_properties(nbajam_ofe PROPERTIES MACOSX_BUNDLE FALSE)
    # SDL for the fingers and the message box: its headers only. The runtime
    # library has SDL in it already, and a second copy here would be a second
    # SDL that never sees the window's events.
    target_link_libraries(nbajam_ofe PRIVATE SDL3::Headers)
    target_link_libraries(nbajam_ofe PRIVATE "-framework Foundation")
endif()

# ReXGlue v0.10.0 translates vpkuwus/vpkuhus by writing the destination
# register one element at a time while still reading the sources, which is
# wrong whenever the destination is also a source. All three vpkuwus128 in
# this image are in the VP6 video decoder and all three alias, which is what
# made the EA Sports intro play with green corruption.
#
# This rewrites those instructions after codegen and before they are compiled.
# It is idempotent and leaves files alone when there is nothing to change, so
# it does not dirty the build. See tools/fix_vector_aliasing.py and BUILDING.md.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(nbajam_ofe_fix_vectors
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/fix_vector_aliasing.py" --apply
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Repairing vector instructions whose destination is also a source"
    VERBATIM)
add_dependencies(nbajam_ofe_fix_vectors nbajam_ofe_codegen)
# nbajam_ofe_recomp only exists once generated/ has sources, so a clean
# checkout configures, runs codegen, then reconfigures.
if(TARGET nbajam_ofe_recomp)
    add_dependencies(nbajam_ofe_recomp nbajam_ofe_fix_vectors)
endif()
