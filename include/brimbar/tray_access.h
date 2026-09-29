#pragma once

#include "brimbar/tray_target.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace brimbar {

struct tray_item final {
    tray_target_kind kind{tray_target_kind::unknown};
    std::wstring automation_id{};
    std::wstring class_name{};
    std::wstring name{};
    RECT source_bounds{};
    bool invokable{};
    bool overflow{};
    int icon_width{};
    int icon_height{};
    std::vector<std::uint32_t> icon_pixels{};
    std::wstring source_icon_path{};
};

enum class tray_activation {
    primary,
    context_menu,
};

[[nodiscard]] std::vector<tray_item> enumerate_tray_items();
[[nodiscard]] std::vector<tray_item> enumerate_available_tray_items();
[[nodiscard]] std::vector<tray_item> enumerate_all_tray_items();
[[nodiscard]] bool invoke_tray_item(const tray_item& item) noexcept;
[[nodiscard]] bool activate_tray_item(
    const tray_item& item,
    tray_activation activation) noexcept;

}  // namespace brimbar
