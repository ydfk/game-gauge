#include "common/config.h"
#include "common/frame_statistics.h"
#include "common/history.h"
#include "common/update_release.h"
#include "metrics/target.h"
#include <fstream>
#include <windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void close(double actual, double expected, const char* message) { require(std::abs(actual - expected) < 0.001, message); }
}
void telemetry_regressions();
int main() {
    try {
        telemetry_regressions();
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
        require(!gauge::Config{}.show_obs && !gauge::config_from_json(gauge::Json::object()).show_obs, "OBS monitoring must default off");
        require(gauge::config_from_json({{"show_obs", true}}).show_obs, "explicit OBS preference must survive upgrades");
        require(gauge::config_from_json({{"metrics", {"disk_temperature"}}}).metrics[0] == "disk_temperature", "disk temperature must be selectable");
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
        game.cpu_load = gauge::available(25, "test");
        game.disk_temperature = gauge::available(43, "test");
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
        close(rows[0]["stats"]["cpu_load"]["average"].get<double>(), 25, "hardware averages must use active time");
        close(rows[0]["stats"]["disk_temperature"]["maximum"].get<double>(), 43, "disk temperature must persist in history");
        require(rows[0].contains("hardware") && rows[0]["series"].size() >= 3, "history must persist hardware and performance curves");
        require(rows[0]["series"][2][1].is_null(), "background intervals must leave gaps in curves");
        require(!gauge::read_history(directory, false)[0].contains("series"), "history list must omit large curve payloads");
        require(gauge::read_history_entry(directory, rows[0]["id"].get<std::string>())["series"] == rows[0]["series"], "history detail must preserve curves");
        {
            gauge::SessionHistory history(directory); auto long_game = game; long_game.target.pid = 888;
            for (uint64_t i = 0; i < 2500; ++i) {
                long_game.timestamp_ms = 1000 + i * 1000;
                long_game.fps = gauge::available(i % 2 ? 80 : 40, "test");
                long_game.cpu_temperature = i == 10 ? gauge::available(90, "test") : gauge::missing(gauge::State::unsupported, "missing");
                history.update(long_game, 1000000 + i * 1000, [](const auto&) { return true; });
            }
            history.finish_all(3500000);
            const auto record = gauge::read_history_entry(directory, "1000000-888");
            require(record["series"].size() <= 1024, "long session curves must be bounded");
            close(record["stats"]["fps"]["minimum"].get<double>(), 40, "decimation must not discard summary minimum");
            close(record["stats"]["fps"]["maximum"].get<double>(), 80, "decimation must not discard summary maximum");
            close(record["stats"]["cpu_temperature"]["average"].get<double>(), 90, "missing sensors must not dilute averages");
            require(record.dump().size() < 262000, "history detail must fit IPC response limit");
            gauge::delete_history(directory, "1000000-888");
        }
        { gauge::SessionHistory history(directory); game.timestamp_ms = 30000;
          history.update(game, 40000, [](const auto&) { return true; }); }
        { gauge::SessionHistory recovery(directory); }
        rows = gauge::read_history(directory);
        require(rows.size() == 2 && rows[0]["status"] == "interrupted", "unclean shutdown must recover history");
        require(gauge::delete_history(directory, rows[0]["id"].get<std::string>()), "history delete must remove selected record");
        require(gauge::read_history(directory).size() == 1, "history delete must preserve other records");
        bool rejected_path = false;
        try { gauge::delete_history(directory, "../outside"); } catch (...) { rejected_path = true; }
        require(rejected_path, "history deletion must reject path traversal");
        { gauge::SessionHistory history(directory); game.timestamp_ms = 50000;
          history.update(game, 50000, [](const auto&) { return true; });
          const auto active_rows = gauge::read_history(directory);
          const auto id = active_rows[0]["id"].get<std::string>();
          gauge::delete_history(directory, id);
          game.timestamp_ms = 60000; history.update(game, 60000, [](const auto&) { return true; });
          history.finish_all(70000);
          for (const auto& row : gauge::read_history(directory)) require(row["id"] != id, "deleted active session must not reappear"); }
        std::filesystem::remove_all(directory);
        gauge::Target browser; browser.pid = 42; browser.name = "chrome.exe"; browser.path = "C:\\Apps\\chrome.exe";
        gauge::Config filtering;
        require(!gauge::target_listed(browser, filtering), "browser must be hidden automatically");
        filtering.known_games.push_back(browser.path);
        require(gauge::target_listed(browser, filtering), "manual game must override automatic filtering");
        filtering.ignored_processes.push_back("CHROME.EXE");
        require(!gauge::target_listed(browser, filtering), "explicit exclusion must match case insensitively");
        const std::string repository = "example/GameGauge";
        gauge::Json release = {{"draft", false}, {"prerelease", false}, {"tag_name", "v1.10.0"}, {"assets", gauge::Json::array({{
            {"name", "GameGauge-1.10.0-Setup.exe"}, {"browser_download_url", "https://github.com/example/GameGauge/releases/download/v1.10.0/GameGauge-1.10.0-Setup.exe"},
            {"size", 123}, {"digest", "sha256:" + std::string(64, 'a')}}})}};
        require(gauge::select_update(release, repository, "1.9.0")["state"] == "available", "version comparison must be numeric");
        require(gauge::select_update(release, repository, "2.0.0")["state"] == "current", "updater must never downgrade");
        const auto rejects_release = [&](gauge::Json invalid) { try { gauge::select_update(invalid, repository, "1.0.0"); return false; } catch (...) { return true; } };
        auto invalid = release; invalid["assets"][0]["browser_download_url"] = "https://example.com/update.exe";
        require(rejects_release(invalid), "foreign update URLs must be rejected");
        invalid = release; invalid["assets"][0]["digest"] = "sha256:bad";
        require(rejects_release(invalid), "missing or malformed digest must be rejected");
        invalid = release; invalid["prerelease"] = true;
        require(rejects_release(invalid), "prerelease must not enter stable update channel");
        invalid = release; invalid["tag_name"] = "v1.2.3/../bad";
        require(rejects_release(invalid), "unsafe version names must be rejected");
        std::cout << "Core, session lifecycle and update contracts passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
