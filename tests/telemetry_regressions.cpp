#include "common/history.h"
#include "common/sample_validation.h"
#include <fstream>
#include <limits>
#include <stdexcept>
#include <windows.h>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void close(double value, double expected, const char* message) { require(std::abs(value - expected) < .001, message); }
}
void telemetry_regressions() {
    require(!gauge::valid_recorded_value("fps", 0), "zero FPS must be excluded from records");
    require(gauge::valid_recorded_value("fps", 1), "real low FPS must remain visible");
    require(gauge::valid_recorded_value("gpu_load", 0), "idle GPU load is valid");
    require(!gauge::valid_recorded_value("gpu_load", 101), "invalid utilization must be rejected");
    require(!gauge::valid_recorded_value("fps", std::numeric_limits<double>::infinity()), "nonfinite samples must be rejected");
    require(!gauge::frame_inside_active_period(102000, 100000, 3000, 1000), "interval spanning background time must be excluded");
    require(!gauge::frame_inside_active_period(99000, 100000, 16, 1000), "queued background frames must be excluded");
    require(gauge::frame_inside_active_period(102000, 100000, 1000, 1000), "real foreground stutters must be preserved");
    const auto root = std::filesystem::temp_directory_path();
    const auto directory = root / (L"GameGauge-telemetry-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    gauge::Snapshot game; game.target = {123, 456, 0, "test.exe", "test.exe", true}; game.game_confirmed = true;
    game.fps = gauge::available(60, "test"); game.cpu_load = gauge::available(0, "test");
    gauge::SessionHistory history(directory);
    const auto update = [&](uint64_t tick) { game.timestamp_ms = tick; history.update(game, 10000 + tick, [](const auto&) { return true; }); };
    update(1000); update(2000);
    close(*history.active_seconds(game.target), 1, "HUD must use the history elapsed time");
    game.target.foreground = false; update(3000); update(15000);
    close(*history.active_seconds(game.target), 1, "background time must not clear or grow elapsed time");
    game.target.foreground = true; update(16000); update(17000);
    close(*history.active_seconds(game.target), 2, "returning to the game must continue its elapsed time");
    const auto original = game.target;
    game.target.pid = 456; game.target.started = 789; update(18000); update(19000);
    close(*history.active_seconds(game.target), 1, "another game needs its own elapsed time");
    game.target = original; update(25000);
    close(*history.active_seconds(game.target), 2, "switching between games must restore the earlier game");
    game.paused = true; update(26000); update(27000); game.paused = false; update(28000); update(29000);
    close(*history.active_seconds(game.target), 3, "pause and resume must preserve accumulated time");
    auto reused = game.target; ++reused.started;
    require(!history.active_seconds(reused), "PID reuse must not restore another process session");
    game.fps = gauge::available(0, "no presents"); update(30000);
    game.fps = gauge::available(20000, "invalid"); game.cpu_load = gauge::available(130, "invalid"); update(31000);
    game.fps = gauge::available(1, "real stutter"); game.cpu_load = gauge::available(0, "idle"); update(32000);
    history.finish_all(45000);
    const auto record = gauge::read_history_entry(directory, "11000-123");
    close(record["stats"]["fps"]["minimum"].get<double>(), 1, "zero samples must not poison the FPS minimum");
    close(record["stats"]["fps"]["maximum"].get<double>(), 60, "invalid spikes must not poison the FPS maximum");
    close(record["stats"]["fps"]["average"].get<double>(), record["average_fps"].get<double>(), "legacy and new averages must use the same valid samples");
    close(record["stats"]["cpu_load"]["minimum"].get<double>(), 0, "valid idle load must not be filtered");
    close(record["stats"]["cpu_load"]["maximum"].get<double>(), 0, "out of range utilization must not enter statistics");
    require(record["filtered_samples"]["fps"] == 2, "filtered FPS samples must be counted");
    require(record["series"].back()[1] == 1, "a real low FPS must stay in the curve");
    auto legacy = record; legacy.erase("sample_policy"); legacy["stats"]["fps"]["minimum"] = 0;
    legacy["series"][0][1] = 0; legacy["latest_low1"] = 0;
    const auto stored = legacy;
    gauge::filter_history_view(legacy);
    require(legacy["stats"]["fps"]["minimum"].is_null() && legacy["series"][0][1].is_null(), "legacy zero values must be hidden");
    require(legacy["legacy_values_hidden"] == true && legacy["latest_low1"].is_null(), "legacy quality must be explicit");
    require(legacy["average_fps"] == stored["average_fps"], "legacy averages cannot be rebuilt from decimated curves");
    require(std::filesystem::weakly_canonical(directory).parent_path() == std::filesystem::weakly_canonical(root), "test cleanup must remain inside its temporary root");
    std::filesystem::remove_all(directory);
}
