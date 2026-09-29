#include "brimbar/tray_target.h"

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool expect_kind(
    const std::wstring_view automation_id,
    const std::wstring_view class_name,
    const brimbar::tray_target_kind expected)
{
    const auto actual = brimbar::classify_tray_target(automation_id, class_name);
    if (actual == expected) {
        return true;
    }
    std::wcerr << L"Unexpected target classification for " << automation_id << L" / " << class_name << L'\n';
    return false;
}

}  // namespace

int main()
{
    const auto all_passed =
        expect_kind(L"NotifyItemIcon", L"SystemTray.NormalButton", brimbar::tray_target_kind::notification_icon) &&
        expect_kind(L"SystemTrayIcon", L"SystemTray.AccentButton", brimbar::tray_target_kind::system_icon) &&
        expect_kind(L"SystemTrayIcon", L"SystemTray.ShowDesktopButton", brimbar::tray_target_kind::show_desktop) &&
        expect_kind(L"StartButton", L"ToggleButton", brimbar::tray_target_kind::unknown);
    return all_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
