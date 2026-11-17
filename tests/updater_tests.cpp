#include "host/updater.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void wait_for(const gauge::Updater& updater, std::string_view state, std::string_view message = {}) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        const auto status = updater.status();
        if (status.value("state", std::string{}) == state &&
            (message.empty() || status.value("message", std::string{}) == message)) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Timed out waiting for updater state: " + std::string(state) + ", got " + updater.status().dump());
}

struct UpdateFixture {
    gauge::Updater::Operations operations() {
        return {
            [this](std::stop_token) {
                ++checks;
                if (current) return gauge::Json{{"state", "current"}};
                return gauge::Json{{"state", "available"}, {"version", "2.0.0"}};
            },
            [this](const gauge::Json&, std::stop_token) { ++downloads; },
            [this](const gauge::Json&) {
                if (invalid) throw std::runtime_error("invalid package");
            },
            [this](const gauge::Json&) {
                ++launches;
                if (cancelled) throw std::runtime_error("cancelled");
                return CreateEventW(nullptr, TRUE, FALSE, nullptr);
            }
        };
    }

    std::atomic<int> checks{}, downloads{}, launches{};
    std::atomic<bool> current{}, invalid{}, cancelled{};
};
}

void updater_contracts() {
    {
        UpdateFixture fixture;
        fixture.cancelled = true;
        gauge::Updater updater(fixture.operations());
        updater.update();
        wait_for(updater, "waiting");
        require(fixture.checks == 1 && fixture.downloads == 1 && fixture.launches == 0,
            "one click must check and download while a game runs");
        updater.update();
        updater.idle(false);
        wait_for(updater, "ready", "cancelled");
        require(fixture.launches == 1, "exiting the game must start one installer");
        fixture.cancelled = false;
        updater.update();
        wait_for(updater, "installing", "已启动安装器");
        require(fixture.checks == 1 && fixture.downloads == 1 && fixture.launches == 2,
            "retrying cancelled installation must reuse the verified package");
    }
    {
        UpdateFixture fixture;
        fixture.invalid = true;
        gauge::Updater updater(fixture.operations());
        updater.idle(false);
        updater.update();
        wait_for(updater, "error", "invalid package");
        require(fixture.launches == 0, "invalid packages must never reach the installer");
        fixture.invalid = false;
        updater.update();
        wait_for(updater, "installing", "已启动安装器");
        require(fixture.checks == 2 && fixture.downloads == 2 && fixture.launches == 1,
            "recovery from invalid package must check and redownload");
    }
    {
        UpdateFixture fixture;
        fixture.current = true;
        gauge::Updater updater(fixture.operations());
        updater.update();
        wait_for(updater, "current");
        require(fixture.checks == 1 && fixture.downloads == 0 && fixture.launches == 0,
            "a current version must stop after the check");
    }
    {
        UpdateFixture fixture;
        gauge::Updater updater(fixture.operations());
        updater.configure(true, false);
        wait_for(updater, "available");
        require(fixture.downloads == 0 && fixture.launches == 0, "periodic checking alone must not download");
        updater.configure(true, true);
        wait_for(updater, "waiting");
        updater.configure(true, false);
        wait_for(updater, "ready");
        updater.idle(false);
        updater.update();
        wait_for(updater, "installing", "已启动安装器");
        require(fixture.downloads == 1 && fixture.launches == 1, "manual update must reuse a package downloaded by automatic updates");
    }
}
