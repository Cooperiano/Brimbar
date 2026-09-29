#include "brimbar/tray_target.h"

namespace brimbar {

std::wstring_view tray_target_kind_name(const tray_target_kind kind) noexcept
{
    switch (kind) {
    case tray_target_kind::notification_icon:
        return L"notification_icon";
    case tray_target_kind::system_icon:
        return L"system_icon";
    case tray_target_kind::show_desktop:
        return L"show_desktop";
    case tray_target_kind::unknown:
    default:
        return L"unknown";
    }
}

}  // namespace brimbar
