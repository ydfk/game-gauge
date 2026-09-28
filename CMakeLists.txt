cmake_minimum_required(VERSION 3.25)
project(GameGauge VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  message(FATAL_ERROR "GameGauge requires Windows")
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()
add_library(gauge_common STATIC
  src/common/config.cpp src/common/frame_statistics.cpp src/common/platform.cpp)
target_include_directories(gauge_common PUBLIC src third_party)
target_link_libraries(gauge_common PUBLIC shell32 advapi32)
add_library(gauge_metrics STATIC
  src/metrics/hardware.cpp src/metrics/nvidia.cpp src/metrics/presentmon.cpp
  src/metrics/sampler.cpp src/metrics/target.cpp)
target_include_directories(gauge_metrics PRIVATE third_party/presentmon)
target_link_libraries(gauge_metrics PUBLIC gauge_common dxgi pdh psapi powrprof dwmapi)
add_executable(GameGauge WIN32
  src/host/main.cpp src/host/application.cpp src/host/ipc_server.cpp
  src/hud/overlay.cpp src/hud/renderer.cpp src/host/app.rc)
target_link_libraries(GameGauge PRIVATE gauge_metrics d3d11 d2d1 dwrite dcomp dwmapi comctl32)
target_link_options(GameGauge PRIVATE /MANIFEST:NO)
add_executable(GameGauge.Diagnostics src/diagnostics/main.cpp src/host/ipc_server.cpp)
target_link_libraries(GameGauge.Diagnostics PRIVATE gauge_metrics)
add_executable(GameGauge.Settings WIN32
  src/settings/main.cpp src/settings/window.cpp src/settings/pages.cpp src/host/ipc_server.cpp)
target_link_libraries(GameGauge.Settings PRIVATE gauge_common d2d1 dwrite windowscodecs)
add_executable(GameGauge.RenderProbe WIN32 tests/render_probe.cpp)
target_link_libraries(GameGauge.RenderProbe PRIVATE d3d11 dxgi)
add_executable(GameGauge.Tests tests/core_tests.cpp)
target_link_libraries(GameGauge.Tests PRIVATE gauge_common)
enable_testing()
add_test(NAME core_contracts COMMAND GameGauge.Tests)
set_target_properties(GameGauge GameGauge.Diagnostics GameGauge.Settings GameGauge.RenderProbe GameGauge.Tests
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
