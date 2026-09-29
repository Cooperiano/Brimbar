#include "brimbar/bridge_api.h"
#include "brimbar/version_policy.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>

namespace {

void copy_message(wchar_t (&destination)[160], const wchar_t* source) noexcept
{
    const auto length = std::min<std::size_t>(std::wcslen(source), std::size(destination) - 1U);
    std::wmemcpy(destination, source, length);
    destination[length] = L'\0';
}

template <std::size_t Size>
void copy_text(wchar_t (&destination)[Size], const std::wstring_view source) noexcept
{
    const auto length = std::min(source.size(), Size - 1U);
    std::copy_n(source.data(), length, destination);
    destination[length] = L'\0';
}

[[nodiscard]] bool is_explorer_process() noexcept
{
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0U || length >= ARRAYSIZE(path)) {
        return false;
    }
    const std::wstring_view full_path{path, length};
    const auto separator = full_path.find_last_of(L"\\/");
    const auto filename = separator == std::wstring_view::npos
        ? full_path
        : full_path.substr(separator + 1U);
    return _wcsicmp(std::wstring{filename}.c_str(), L"explorer.exe") == 0;
}

void publish_runtime_state() noexcept
{
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, brimbar::bridge_mapping_name);
    if (mapping == nullptr) {
        return;
    }
    auto* state = static_cast<brimbar::bridge_runtime_state*>(MapViewOfFile(
        mapping, FILE_MAP_WRITE, 0, 0, sizeof(brimbar::bridge_runtime_state)));
    if (state == nullptr) {
        CloseHandle(mapping);
        return;
    }

    brimbar::bridge_runtime_state next{};
    next.structure_size = sizeof(next);
    next.magic = brimbar::bridge_protocol_magic;
    next.protocol_version = brimbar::bridge_protocol_version;
    next.explorer_process_id = GetCurrentProcessId();
    next.taskbar_thread_id = GetCurrentThreadId();
    const auto version = brimbar::current_windows_version();
    next.windows_build = version.build;

    if (!is_explorer_process()) {
        next.phase = brimbar::bridge_runtime_phase::rejected_process;
        copy_text(next.diagnostic, L"Hook did not execute inside explorer.exe.");
    } else if (version.build != 26200U) {
        next.phase = brimbar::bridge_runtime_phase::rejected_build;
        copy_text(next.diagnostic, L"Exact Windows build is not approved for the native bridge.");
    } else {
        HMODULE system_tray = GetModuleHandleW(L"SystemTray.dll");
        std::wstring_view module_name{L"SystemTray.dll"};
        if (system_tray == nullptr) {
            system_tray = GetModuleHandleW(L"Taskbar.View.dll");
            module_name = L"Taskbar.View.dll";
        }
        if (system_tray == nullptr) {
            next.phase = brimbar::bridge_runtime_phase::module_missing;
            copy_text(next.diagnostic, L"Explorer is present, but no supported tray module is loaded.");
        } else {
            next.phase = brimbar::bridge_runtime_phase::read_only_ready;
            next.system_tray_module = reinterpret_cast<std::uintptr_t>(system_tray);
            copy_text(next.system_tray_module_name, module_name);
            copy_text(next.diagnostic, L"Explorer bridge is attached in read-only mode.");
        }
    }

    *state = next;
    FlushViewOfFile(state, sizeof(*state));
    UnmapViewOfFile(state);
    CloseHandle(mapping);
}

}  // namespace

extern "C" bool __stdcall brimbar_bridge_probe(brimbar::bridge_probe_result* result) noexcept
{
    if (result == nullptr || result->structure_size != sizeof(brimbar::bridge_probe_result)) {
        return false;
    }

    const auto version = brimbar::current_windows_version();
    result->windows_build = version.build;
    result->candidate_build = brimbar::is_candidate_build(version.build);
    result->implementation_ready = false;

    copy_message(
        result->message,
        result->candidate_build
            ? L"Build accepted for offline symbol probing; Explorer mutation remains disabled."
            : L"Unsupported Windows build; bridge will fail open without modifying Explorer.");
    return true;
}

extern "C" LRESULT CALLBACK BrimbarBridgeHookProc(
    const int code,
    const WPARAM word_parameter,
    const LPARAM long_parameter) noexcept
{
    if (code >= 0) {
        publish_runtime_state();
    }
    return CallNextHookEx(nullptr, code, word_parameter, long_parameter);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
