#include "components.h"
#include "common/platform.h"
#include <objbase.h>
#include <shellapi.h>

namespace gauge {
ComponentMonitor::ComponentMonitor() : worker_([this](std::stop_token stop) { run(stop); }) {}
ComponentMonitor::~ComponentMonitor() { worker_.request_stop(); wake_.notify_all(); }
Json ComponentMonitor::status(const Snapshot& snapshot) const {
    std::lock_guard lock(mutex_);
    return {{"items", component_readings(components_, snapshot)}, {"repair_state", state_},
        {"message", message_}, {"can_repair", component_repair_available()}};
}
void ComponentMonitor::repair() {
    std::lock_guard lock(mutex_);
    if (state_ == "running") return;
    if (!component_repair_available()) throw std::runtime_error("缺少离线修复资源，请安装新版完整安装包");
    requested_ = true; state_ = "running"; message_ = "等待管理员授权并修复采集组件…";
    wake_.notify_all();
}
void ComponentMonitor::run(std::stop_token stop) {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    UniqueHandle process;
    while (!stop.stop_requested()) {
        try {
            bool requested{};
            { std::lock_guard lock(mutex_); requested = requested_; requested_ = false; }
            if (requested) {
                if (!component_repair_available()) throw std::runtime_error("离线修复资源缺失，请重新安装游戏仪表");
                wchar_t system[32768]{}; GetSystemDirectoryW(system, 32768);
                const auto shell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
                const auto script = executable_dir() / L"scripts/repair-components.ps1";
                const auto arguments = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"";
                SHELLEXECUTEINFOW execute{sizeof(execute)};
                execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
                execute.lpVerb = L"runas"; execute.lpFile = shell.c_str(); execute.lpParameters = arguments.c_str(); execute.nShow = SW_HIDE;
                if (!ShellExecuteExW(&execute)) {
                    const auto error = GetLastError();
                    throw std::runtime_error(error == ERROR_CANCELLED ? "已取消管理员授权，未执行修复" : "无法启动组件修复：" + error_text(error));
                }
                if (!execute.hProcess) throw std::runtime_error("无法跟踪修复进程，请查看组件状态");
                process.reset(execute.hProcess);
            }
            if (process && WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) {
                DWORD code{};
                if (!GetExitCodeProcess(process.value, &code)) throw std::runtime_error("无法读取修复结果");
                process.reset();
                std::lock_guard lock(mutex_);
                state_ = code == 0 ? "complete" : code == 3010 ? "reboot" : "error";
                message_ = code == 0 ? "组件修复完成，正在检查实时读数" : code == 3010 ? "组件已安装，请重启 Windows 完成修复" :
                    "修复失败（" + std::to_string(code) + "），请查看修复日志或重新安装";
            }
            auto components = inspect_components();
            { std::lock_guard lock(mutex_); components_ = std::move(components); }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_); state_ = "error"; message_ = error.what();
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(2), [&] { return requested_ || stop.stop_requested(); });
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
}
