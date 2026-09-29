#include "appbar.h"

#include <Shellapi.h>

namespace brimbar {

appbar::~appbar() noexcept
{
    detach();
}

bool appbar::attach(const HWND window, const int height_pixels) noexcept
{
    if (window == nullptr || height_pixels <= 0) {
        return false;
    }

    window_ = window;
    height_pixels_ = height_pixels;

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uCallbackMessage = callback_message;
    registered_ = SHAppBarMessage(ABM_NEW, &data) != 0;
    if (registered_) {
        reposition();
    }
    return registered_;
}

void appbar::reposition() noexcept
{
    if (!registered_) {
        return;
    }

    const auto monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        return;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uEdge = ABE_TOP;
    data.rc = monitor_info.rcMonitor;
    data.rc.bottom = data.rc.top + height_pixels_;

    static_cast<void>(SHAppBarMessage(ABM_QUERYPOS, &data));
    data.rc.bottom = data.rc.top + height_pixels_;
    static_cast<void>(SHAppBarMessage(ABM_SETPOS, &data));

    SetWindowPos(
        window_, HWND_TOPMOST, data.rc.left, data.rc.top,
        data.rc.right - data.rc.left, data.rc.bottom - data.rc.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void appbar::notify_window_position_changed() const noexcept
{
    if (!registered_) {
        return;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    static_cast<void>(SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data));
}

void appbar::detach() noexcept
{
    if (!registered_) {
        return;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    static_cast<void>(SHAppBarMessage(ABM_REMOVE, &data));
    registered_ = false;
    window_ = nullptr;
}

}  // namespace brimbar
