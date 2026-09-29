#pragma once

#include <string>
#include <string_view>

namespace brimbar {

[[nodiscard]] std::wstring process_icon_key(std::wstring_view tray_name);

[[nodiscard]] int tray_identity_match_score(
    std::wstring_view cached_name,
    std::wstring_view cached_icon_path,
    std::wstring_view current_name,
    std::wstring_view current_icon_path);

}  // namespace brimbar
