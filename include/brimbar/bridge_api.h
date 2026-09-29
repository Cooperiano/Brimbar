#pragma once

#include <cstdint>
#include <Windows.h>

#if defined(BRIMBAR_BRIDGE_EXPORTS)
#define BRIMBAR_BRIDGE_API __declspec(dllexport)
#else
#define BRIMBAR_BRIDGE_API __declspec(dllimport)
#endif

namespace brimbar {

inline constexpr std::uint32_t bridge_protocol_magic{0x4252494DU};  // BRIM
inline constexpr std::uint32_t bridge_protocol_version{1U};
inline constexpr wchar_t bridge_mapping_name[]{L"Local\\Brimbar.Bridge.v1"};

enum class bridge_runtime_phase : std::uint32_t {
    empty,
    rejected_process,
    rejected_build,
    module_missing,
    read_only_ready,
};

struct bridge_runtime_state final {
    std::uint32_t structure_size{};
    std::uint32_t magic{};
    std::uint32_t protocol_version{};
    bridge_runtime_phase phase{bridge_runtime_phase::empty};
    std::uint32_t explorer_process_id{};
    std::uint32_t taskbar_thread_id{};
    std::uint32_t windows_build{};
    std::uintptr_t system_tray_module{};
    wchar_t system_tray_module_name[32]{};
    wchar_t diagnostic[160]{};
};

struct bridge_probe_result final {
    std::uint32_t structure_size{};
    std::uint32_t windows_build{};
    bool candidate_build{};
    bool implementation_ready{};
    wchar_t message[160]{};
};

}  // namespace brimbar

extern "C" BRIMBAR_BRIDGE_API bool __stdcall brimbar_bridge_probe(
    brimbar::bridge_probe_result* result) noexcept;

extern "C" BRIMBAR_BRIDGE_API LRESULT CALLBACK BrimbarBridgeHookProc(
    int code,
    WPARAM word_parameter,
    LPARAM long_parameter) noexcept;

#undef BRIMBAR_BRIDGE_API
