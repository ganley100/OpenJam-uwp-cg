# OpenJam Xbox smoke test: a UWP app built with clang-cl. See main.cpp.
#
# Configure from a VS developer prompt set up for UWP (vcvarsall x64 uwp), so
# LIB points at MSVC's app-container CRT in lib\x64\store.

cmake_minimum_required(VERSION 3.25)
project(openjam_xbox_smoketest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

# The desktop defaults (kernel32, user32, gdi32 ...) are not what an app links:
# WindowsApp.lib is the umbrella for every API an app may call.
set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib")

add_executable(smoketest WIN32 main.cpp font5x7.h)
target_compile_definitions(smoketest PRIVATE
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(smoketest PRIVATE /EHsc /W3)
target_link_options(smoketest PRIVATE /APPCONTAINER)
target_link_libraries(smoketest PRIVATE d3d12.lib dxgi.lib xinputuap.lib)
