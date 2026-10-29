cmake_minimum_required(VERSION 3.22)
# v0.19: THIS project() VERSION IS THE SINGLE SOURCE OF TRUTH for the
# app version (kit v0.19 == 0.19.0; v0.19.1 == 0.19.1). It feeds the
# generated MeterBarVersion.h (UI string, splash, application version,
# updater), the juce_add_gui_app version in app/CMakeLists.txt, and —
# via build-release.bat / CI passing ISCC /DMyAppVersion — the
# installer. Before v0.19 the version was hard-coded in six places and
# they were guaranteed to drift; do not add a seventh. Bump it here,
# only here.
project(MeterBar VERSION 0.20.0 LANGUAGES C CXX)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/app/MeterBarVersion.h.in
               ${CMAKE_BINARY_DIR}/generated/MeterBarVersion.h)
# v0.15 distribution: link the MSVC runtime STATICALLY (/MT release,
# /MTd debug) so a built MeterBar.exe runs on a friend's PC with NO
# Visual C++ Redistributable installed. Must be set before any target
# is created (CMP0091). MSVC-only; inert on other toolchains. If a
# future JUCE version ever conflicts with this, the fallback is dynamic
# linking + the vc_redist bundled by MeterBar.iss (see README_TEST).
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(METERBAR_BUILD_APP "Build the JUCE GUI app" OFF)

# ---- Phase A: pure DSP core, zero external deps ----
add_library(meterbar_dsp INTERFACE)
target_include_directories(meterbar_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)
if(UNIX AND NOT APPLE)
  target_link_libraries(meterbar_dsp INTERFACE m)
endif()

enable_testing()
add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE meterbar_dsp)
add_test(NAME dsp COMMAND test_dsp)

# ---- Phase B: JUCE app ----
if(METERBAR_BUILD_APP)
  include(FetchContent)
  # URL (not git): Jackson's Windows PC has no git installed; CMake can
  # download+extract a release archive itself. (Kit fix v0.5, 2026-10-13 —
  # his build-log-2.txt: "could not find git for clone of juce-populate".)
  FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(JUCE)
  add_subdirectory(app)
endif()
