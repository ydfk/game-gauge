#include "common/config.h"
#include "common/frame_statistics.h"
#include "common/history.h"
#include <fstream>
#include <windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void close(double actual, double expected, const char* message) { require(std::abs(actual - expected) < 0.001, message); }
}
int main() {
    try {
        std::vector<double> values(990, 10); values.insert(values.end(), 10, 50);
        close(gauge::slow_tail_fps(values, .01), 20, "1% Low must average the slowest frame times");
        close(gauge::slow_tail_fps({10, 20, 30}, .01), 1000.0 / 30, "small samples use ceil");
        gauge::FrameStatistics statistics;
        for (uint64_t i = 0; i < 1000; ++i) statistics.add(1000 + i, 11, 10);
        for (uint64_t i = 0; i < 100; ++i) statistics.add(1900 + i, 22, 20);
        statistics.add(2000, 11, std::numeric_limits<double>::quiet_NaN());
        gauge::Snapshot snapshot; statistics.publish(snapshot, 2000);
        close(*snapshot.fps.value, 100, "secondary swap chain must not inflate FPS");
        close(*snapshot.low1.value, 100, "stable frame stream low must match FPS");
        require(!snapshot.low01.value && snapshot.low01.state == gauge::State::waiting, "0.1% requires enough samples");
        statistics.publish(snapshot, 3200);
        close(*snapshot.fps.value, 100, "short frame delivery gaps must keep the last FPS");
        statistics.publish(snapshot, 4201);
        require(!snapshot.fps.value && snapshot.fps.state == gauge::State::waiting,
            "FPS must become unavailable after the frame stream stops");
        statistics.publish(snapshot, 64000);
        require(!snapshot.fps.value, "old frame data must not remain live");
        statistics.reset();
        statistics.add(1000, 1, 10);
        statistics.add(3000, 1, 100);
        statistics.publish(snapshot, 1500);
        close(*snapshot.fps.value, 100, "future frames must not affect current FPS");
        auto config = gauge::config_from_json({{"version", 1}, {"font_size", 100}, {"refresh_ms", 1},
            {"metrics", {"fps", "unknown", "fps", "cpu_load"}}});
        close(config.font_size, 32, "font size must be bounded");
        require(config.refresh_ms == 100, "sampling interval must be bounded");
        require(config.metrics.size() == 2, "metric IDs must be known and unique");
        const auto serialized = gauge::config_json(config);
        require(gauge::config_json(gauge::config_from_json(serialized)) == serialized, "config round trip must preserve values");
        bool rejected{};
        try { gauge::config_from_json({{"version", 999}}); } catch (...) { rejected = true; }
        require(rejected, "future incompatible config version must be rejected");
        snapshot.cpu_temperature = gauge::missing(gauge::State::unsupported, "not supported");
        const auto json = gauge::snapshot_json(snapshot);
        require(json["cpu_temperature"]["value"].is_null(), "missing sensors must not become zero");
        const auto directory = std::filesystem::temp_directory_path() / (L"GameGauge-history-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        gauge::Snapshot game; game.target.pid = 123; game.target.started = 456;
        game.target.name = "test.exe"; game.target.foreground = true; game.game_confirmed = true;
        game.fps = gauge::available(60, "test");
        { gauge::SessionHistory history(directory);
          game.timestamp_ms = 1000; history.update(game, 10000, [](const auto&) { return true; });
          game.timestamp_ms = 2000; history.update(game, 11000, [](const auto&) { return true; });
          game.target.foreground = false; game.timestamp_ms = 3000;
          history.update(game, 12000, [](const auto&) { return true; });
          game.target.foreground = true; game.timestamp_ms = 20000;
          history.update(game, 29000, [](const auto&) { return true; });
          gauge::Snapshot desktop;
          history.update(desktop, 30000, [](const auto&) { return false; }); }
        auto rows = gauge::read_history(directory);
        require(rows.size() == 1 && rows[0]["status"] == "completed", "game exit must complete one local session");
        close(rows[0]["active_seconds"].get<double>(), 1, "background time must not inflate active time");
        close(rows[0]["average_fps"].get<double>(), 60, "session FPS aggregation");
        { gauge::SessionHistory history(directory); game.timestamp_ms = 30000;
          history.update(game, 40000, [](const auto&) { return true; }); }
        { gauge::SessionHistory recovery(directory); }
        rows = gauge::read_history(directory);
        require(rows.size() == 2 && rows[0]["status"] == "interrupted", "unclean shutdown must recover history");
        std::filesystem::remove_all(directory);
        std::cout << "Core and session lifecycle contracts passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
