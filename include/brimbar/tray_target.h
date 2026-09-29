#pragma once

#include <string_view>

namespace brimbar {

enum class tray_target_kind {
    unknown,
    notification_icon,
    system_icon,
    show_desktop,
};

[[nodiscard]] constexpr tray_target_kind classify_tray_target(
    const std::wstring_view automation_id,
    const std::wstring_view class_name) noexcept
{
    if (automation_id == L"NotifyItemIcon") {
        return tray_target_kind::notification_icon;
    }
    if (automation_id != L"SystemTrayIcon") {
        return tray_target_kind::unknown;
    }
    if (class_name == L"SystemTray.ShowDesktopButton") {
        return tray_target_kind::show_desktop;
    }
    return tray_target_kind::system_icon;
}

[[nodiscard]] std::wstring_view tray_target_kind_name(tray_target_kind kind) noexcept;

}  // namespace brimbar
