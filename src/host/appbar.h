#pragma once

#include <Windows.h>

namespace brimbar {

class appbar final {
public:
    static constexpr UINT callback_message{WM_APP + 42U};

    appbar() = default;
    ~appbar() noexcept;

    appbar(const appbar&) = delete;
    appbar& operator=(const appbar&) = delete;
    appbar(appbar&&) = delete;
    appbar& operator=(appbar&&) = delete;

    [[nodiscard]] bool attach(HWND window, int height_pixels) noexcept;
    void reposition() noexcept;
    void detach() noexcept;

private:
    HWND window_{};
    int height_pixels_{};
    bool registered_{};
};

}  // namespace brimbar
