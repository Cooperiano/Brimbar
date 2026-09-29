#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace brimbar {

// A copied snapshot from Explorer's native notification-area registry.
// No pointer or icon handle returned by Explorer escapes the callback.
struct native_tray_item final {
    std::wstring executable_path{};
    std::wstring tooltip{};
    std::uintptr_t owner_window{};
    std::uint32_t identifier{};
    GUID guid{};
    std::uint32_t preference{};
    std::uint32_t event{};
    bool active_window{};
    int icon_width{};
    int icon_height{};
    std::vector<std::uint32_t> icon_pixels{};
};

struct native_tray_snapshot final {
    HRESULT result{E_FAIL};
    bool interface_available{};
    std::uint32_t callback_count{};
    std::vector<native_tray_item> items{};
};

// Uses Explorer's undocumented ITrayNotify/INotificationCB COM contract.
// The call is read-only: it registers a callback, copies the synchronous
// snapshot, and immediately unregisters.
[[nodiscard]] native_tray_snapshot enumerate_native_tray_items() noexcept;

}  // namespace brimbar
