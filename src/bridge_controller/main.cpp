#include "brimbar/bridge_api.h"
#include "brimbar/version_policy.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

class handle final {
public:
    explicit handle(HANDLE value = nullptr) noexcept : value_{value} {}
    ~handle() noexcept
    {
        if (value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_{};
};

[[nodiscard]] bool taskbar_belongs_to_explorer(HWND taskbar) noexcept
{
    DWORD process_id{};
    if (GetWindowThreadProcessId(taskbar, &process_id) == 0U || process_id == 0U) {
        return false;
    }
    const handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
    if (process.get() == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    DWORD length{ARRAYSIZE(path)};
    if (QueryFullProcessImageNameW(process.get(), 0, path, &length) == FALSE) {
        return false;
    }
    return _wcsicmp(std::filesystem::path{std::wstring_view{path, length}}.filename().c_str(), L"explorer.exe") == 0;
}

[[nodiscard]] const wchar_t* phase_name(const brimbar::bridge_runtime_phase phase) noexcept
{
    switch (phase) {
    case brimbar::bridge_runtime_phase::empty:
        return L"empty";
    case brimbar::bridge_runtime_phase::rejected_process:
        return L"rejected-process";
    case brimbar::bridge_runtime_phase::rejected_build:
        return L"rejected-build";
    case brimbar::bridge_runtime_phase::module_missing:
        return L"module-missing";
    case brimbar::bridge_runtime_phase::read_only_ready:
        return L"read-only-ready";
    }
    return L"unknown";
}

}  // namespace

int wmain()
{
    const auto version = brimbar::current_windows_version();
    if (version.build != 26200U) {
        std::wcerr << L"Native bridge refused unsupported build " << version.build << L'\n';
        return 10;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr || !taskbar_belongs_to_explorer(taskbar)) {
        std::wcerr << L"Verified Explorer taskbar was not found\n";
        return 11;
    }

    const handle mapping{CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(brimbar::bridge_runtime_state),
        brimbar::bridge_mapping_name)};
    if (mapping.get() == nullptr) {
        std::wcerr << L"Bridge shared memory creation failed: " << GetLastError() << L'\n';
        return 12;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::wcerr << L"Bridge shared memory already exists; refusing a spoofable handshake\n";
        return 12;
    }
    auto* state = static_cast<brimbar::bridge_runtime_state*>(MapViewOfFile(
        mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(brimbar::bridge_runtime_state)));
    if (state == nullptr) {
        std::wcerr << L"Bridge shared memory mapping failed: " << GetLastError() << L'\n';
        return 13;
    }
    *state = {};
    state->structure_size = sizeof(*state);

    wchar_t executable_path[MAX_PATH]{};
    const auto executable_length = GetModuleFileNameW(nullptr, executable_path, ARRAYSIZE(executable_path));
    const auto bridge_path = std::filesystem::path{
        std::wstring_view{executable_path, executable_length}}.parent_path() / L"brimbar_explorer_bridge.dll";
    const HMODULE bridge = LoadLibraryExW(bridge_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (bridge == nullptr) {
        std::wcerr << L"Bridge DLL load failed: " << GetLastError() << L'\n';
        UnmapViewOfFile(state);
        return 14;
    }
    const auto hook_procedure = reinterpret_cast<HOOKPROC>(
        GetProcAddress(bridge, "BrimbarBridgeHookProc"));
    DWORD explorer_process{};
    const DWORD taskbar_thread = GetWindowThreadProcessId(taskbar, &explorer_process);
    const HHOOK hook = hook_procedure == nullptr
        ? nullptr
        : SetWindowsHookExW(WH_CALLWNDPROC, hook_procedure, bridge, taskbar_thread);
    if (hook == nullptr) {
        std::wcerr << L"Explorer bridge hook failed: " << GetLastError() << L'\n';
        FreeLibrary(bridge);
        UnmapViewOfFile(state);
        return 15;
    }

    SendMessageTimeoutW(taskbar, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
    UnhookWindowsHookEx(hook);

    const auto snapshot = *state;
    UnmapViewOfFile(state);
    FreeLibrary(bridge);

    std::wcout << L"phase=" << phase_name(snapshot.phase)
               << L" explorer_pid=" << snapshot.explorer_process_id
               << L" taskbar_tid=" << snapshot.taskbar_thread_id
               << L" build=" << snapshot.windows_build
               << L" module=" << snapshot.system_tray_module_name
               << L" base=0x" << std::hex << snapshot.system_tray_module << std::dec
               << L" diagnostic=\"" << snapshot.diagnostic << L"\"\n";

    const bool valid = snapshot.structure_size == sizeof(snapshot) &&
        snapshot.magic == brimbar::bridge_protocol_magic &&
        snapshot.protocol_version == brimbar::bridge_protocol_version &&
        snapshot.explorer_process_id == explorer_process &&
        snapshot.taskbar_thread_id == taskbar_thread &&
        snapshot.phase == brimbar::bridge_runtime_phase::read_only_ready;
    return valid ? EXIT_SUCCESS : 16;
}
