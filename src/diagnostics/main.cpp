#include "common/config.h"
#include "common/platform.h"
#include "metrics/sampler.h"
#include "metrics/target.h"
#include "metrics/presentmon.h"
#include "metrics/hardware.h"
#include "host/ipc_server.h"
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        gauge::Config config;
        unsigned duration = 2500;
        uint32_t presentmon_pid{};
        bool show_capabilities{};
        for (int i = 1; i < argc; ++i) {
            std::wstring argument = argv[i];
            if (argument == L"--data-dir" && i + 1 < argc) gauge::set_data_dir(argv[++i]);
            else if (argument == L"--target-pid" && i + 1 < argc) {
                config.auto_target = false; config.hide_on_blur = false; config.target_pid = std::stoul(argv[++i]);
            }
            else if (argument == L"--sample-ms" && i + 1 < argc) duration = std::clamp(std::stoul(argv[++i]), 1000ul, 60000ul);
            else if (argument == L"--presentmon-probe" && i + 1 < argc) presentmon_pid = std::stoul(argv[++i]);
            else if (argument == L"--presentmon-capabilities") show_capabilities = true;
            else if (argument == L"--host-status") { std::cout << gauge::ipc_request({{"command", "status"}}).dump(2) << '\n'; return 0; }
            else if (argument == L"--request" && i + 1 < argc) { std::cout << gauge::ipc_request(gauge::Json::parse(gauge::utf8(argv[++i]))).dump(2) << '\n'; return 0; }
            else if (argument == L"--targets") {
                gauge::Json targets = gauge::Json::array();
                for (const auto& target : gauge::enumerate_targets()) targets.push_back({{"pid", target.pid}, {"name", target.name}});
                std::cout << targets.dump(2) << '\n'; return 0;
            }
        }
        if (show_capabilities) {
            gauge::PresentMonProvider provider;
            gauge::Json capabilities = gauge::Json::array();
            for (const auto& item : provider.capabilities()) capabilities.push_back({
                {"metric", item.metric}, {"device", item.device}, {"device_luid", item.device_luid},
                {"availability", item.availability}});
            std::cout << gauge::Json{{"frame_status", provider.status()}, {"capabilities", capabilities}}.dump(2) << '\n';
            return capabilities.empty() ? 1 : 0;
        }
        if (presentmon_pid) {
            gauge::Target target; target.pid = presentmon_pid;
            gauge::Snapshot snapshot; snapshot.hardware = gauge::discover_hardware();
            gauge::FrameStatistics statistics; gauge::PresentMonProvider provider;
            const auto began = GetTickCount64(), until = began + duration;
            uint32_t polls{}, valid_polls{}; uint64_t first_frame_ms{};
            double minimum_fps = 100000, maximum_fps{};
            do {
                provider.poll(target, snapshot, statistics);
                statistics.publish(snapshot, GetTickCount64());
                ++polls;
                if (snapshot.fps.state == gauge::State::valid && snapshot.fps.value) {
                    if (!valid_polls) first_frame_ms = GetTickCount64() - began;
                    ++valid_polls;
                    minimum_fps = std::min(minimum_fps, *snapshot.fps.value);
                    maximum_fps = std::max(maximum_fps, *snapshot.fps.value);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            } while (GetTickCount64() < until);
            std::cout << gauge::Json{{"frame_status", provider.status()}, {"raw_frames", provider.raw_frame_count()},
                {"accepted_frames", provider.accepted_frame_count()}, {"fps", gauge::snapshot_json(snapshot)["fps"]},
                {"polls", polls}, {"valid_polls", valid_polls}, {"first_frame_ms", valid_polls ? gauge::Json(first_frame_ms) : gauge::Json(nullptr)},
                {"minimum_fps", valid_polls ? gauge::Json(minimum_fps) : gauge::Json(nullptr)}, {"maximum_fps", valid_polls ? gauge::Json(maximum_fps) : gauge::Json(nullptr)},
                {"frame_samples", snapshot.frame_samples}, {"cpu_temperature", gauge::snapshot_json(snapshot)["cpu_temperature"]}}.dump(2) << '\n';
            return provider.accepted_frame_count() ? 0 : 2;
        }
        gauge::Sampler sampler(config);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
        auto snapshot = sampler.snapshot();
        std::cout << gauge::snapshot_json(snapshot).dump(2) << '\n';
        return snapshot.hardware.cpu.empty() ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
