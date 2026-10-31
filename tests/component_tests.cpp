#include "common/components.h"
#include "host/components.h"
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
void component_contracts() {
    gauge::Snapshot snapshot;
    snapshot.hardware.cpu_vendor = "AuthenticAMD";
    snapshot.target.pid = 123; snapshot.target.foreground = true;
    const gauge::Json components = gauge::Json::array({
        {{"id", "presentmon"}, {"state", "ready"}},
        {{"id", "pawnio"}, {"state", "missing"}, {"detail", "driver missing"}},
        {{"id", "sensor"}, {"state", "ready"}}});
    snapshot.fps = gauge::available(144, "test");
    snapshot.cpu_temperature = gauge::missing(gauge::State::error, "read failed");
    auto report = gauge::component_readings(components, snapshot);
    require(report[0]["state"] == "ready", "valid game frames remain healthy");
    require(report[1]["state"] == "missing", "missing driver must not be hidden by running sensor");
    require(report[2]["state"] == "error" && report[2]["detail"] == "read failed", "running service with invalid temperature is unhealthy");
    snapshot.cpu_temperature = gauge::available(65, "test");
    report = gauge::component_readings(components, snapshot);
    require(report[2]["state"] == "ready", "fresh reading clears sampling error");
    snapshot.cpu_temperature = gauge::available(std::numeric_limits<double>::quiet_NaN(), "test");
    require(gauge::component_readings(components, snapshot)[2]["state"] == "error", "nonfinite temperature is not healthy");
    snapshot.target.pid = 0;
    snapshot.fps = gauge::missing(gauge::State::waiting, "no game");
    require(gauge::component_readings(components, snapshot)[0]["state"] == "ready", "idle desktop is not a broken FPS dependency");
    snapshot.target.pid = 123;
    snapshot.fps = gauge::missing(gauge::State::error, "disconnected");
    require(gauge::component_readings(components, snapshot)[0]["state"] == "error", "game with broken stream exposes error");
    snapshot.fps = gauge::missing(gauge::State::waiting, "PresentMon 服务不可用，状态 12");
    require(gauge::component_readings(components, snapshot)[0]["state"] == "error", "connection failures must not remain a silent waiting state");
    snapshot.fps = gauge::missing(gauge::State::waiting, "帧事件无有效样本 · 已自动重连 1 次");
    require(gauge::component_readings(components, snapshot)[0]["state"] == "error", "unusable raw frames must surface as an error");
    snapshot.target.foreground = false;
    require(gauge::component_readings(components, snapshot)[0]["state"] == "ready", "background game does not trigger false alarm");
    snapshot.hardware.cpu_vendor = "GenuineIntel";
    report = gauge::component_readings(components, snapshot);
    require(report[1]["state"] == "unsupported" && report[2]["state"] == "unsupported", "AMD-only dependencies are not required on Intel");
    gauge::ComponentMonitor monitor;
    gauge::Json status;
    for (int attempt = 0; attempt < 50; ++attempt) {
        status = monitor.status(snapshot);
        if (status["items"].size() == 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(status["items"].size() == 3, "background monitor must publish three component states");
    if (!status["can_repair"].get<bool>()) {
        bool rejected{};
        try { monitor.repair(); } catch (const std::exception&) { rejected = true; }
        require(rejected, "repair must not elevate an uninstalled or incomplete build");
    }
}
