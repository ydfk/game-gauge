#pragma once
#include "common/config.h"
#include "metrics/sampler.h"
#include "hud/overlay.h"
#include "ipc_server.h"
#include "updater.h"
#include "obs.h"
#include "components.h"
#include "saves.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <shellapi.h>
namespace gauge {
class Application {
public:
    Application(HINSTANCE instance, Config config, bool show_settings, uint32_t timed_exit);
    ~Application();
    int run();
private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    Json request(const Json& command);
    void add_tray();
    void menu();
    void settings(bool backup_page = false);
    void backup_from_tray();
    void refresh_saves();
    void saves_notice(std::wstring message, bool error);
    void quit_when_safe();
    void update_config(Config config);
    void export_session();
    Config config() const;
    Snapshot snapshot() const;
    HINSTANCE instance_{};
    HWND window_{};
    HICON icon_{};
    UINT taskbar_created_{};
    mutable std::mutex mutex_;
    Config config_;
    Sampler sampler_;
    Updater updater_;
    ObsMonitor obs_;
    ComponentMonitor components_;
    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<IpcServer> ipc_;
    std::unique_ptr<Saves> saves_;
    std::string saves_hotkey_, saves_tray_job_;
    std::atomic<bool> saves_hotkey_failed_{};
    std::atomic<bool> capture_requested_{};
    std::atomic<DWORD> capture_error_{};
    std::string hud_error_, warning_;
    uint64_t component_issue_since_{};
    bool component_notified_{};
    bool show_settings_{};
    bool pending_exit_{};
    uint32_t timed_exit_{};
};
}
