#include "brimbar/process_icon_match.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace {

[[nodiscard]] std::wstring lowercase(const std::wstring_view value)
{
    std::wstring result{value};
    std::ranges::transform(result, result.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

[[nodiscard]] std::wstring stable_label(const std::wstring_view value)
{
    auto result = lowercase(value);
    const auto start = result.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) {
        return {};
    }
    result.erase(0U, start);
    const auto separator = result.find_first_of(L":：\r\n");
    if (separator != std::wstring::npos) {
        result.resize(separator);
    }
    while (!result.empty() && std::iswspace(result.back())) {
        result.pop_back();
    }
    return result;
}

}  // namespace

namespace brimbar {

std::wstring process_icon_key(const std::wstring_view tray_name)
{
    const auto name = lowercase(tray_name);
    constexpr std::array rules{
        std::pair{L"企业微信", L"wxwork"},
        std::pair{L"onedrive", L"onedrive"},
        std::pair{L"workbuddy", L"workbuddy"},
        std::pair{L"tailscale", L"tailscale-ipn"},
        std::pair{L"microsoft 365 copilot", L"m365copilot"},
        std::pair{L"微软电脑管家", L"mspcmanager"},
        std::pair{L"微信", L"weixin"},
        std::pair{L"nvidia", L"nvidia"},
        std::pair{L"网易uu远程", L"gameviewer"},
        std::pair{L"g-helper", L"ghelper"},
        std::pair{L"inktyper", L"inktyper"},
        std::pair{L"chatgpt", L"chatgpt"},
        std::pair{L"clash verge", L"clash-verge"},
        std::pair{L"zcode", L"zcode"},
        std::pair{L"windows 安全中心", L"securityhealthsystray"},
    };
    for (const auto& [label, process] : rules) {
        if (name.find(label) != std::wstring::npos) {
            return process;
        }
    }
    return {};
}

int tray_identity_match_score(
    const std::wstring_view cached_name,
    const std::wstring_view cached_icon_path,
    const std::wstring_view current_name,
    const std::wstring_view current_icon_path)
{
    if (!cached_icon_path.empty() && !current_icon_path.empty() &&
        lowercase(cached_icon_path) == lowercase(current_icon_path)) {
        return 100;
    }

    const auto cached_process = process_icon_key(cached_name);
    const auto current_process = process_icon_key(current_name);
    if (!cached_process.empty() && cached_process == current_process) {
        return 90;
    }

    if (!cached_name.empty() && cached_name == current_name) {
        return 80;
    }

    const auto cached_label = stable_label(cached_name);
    const auto current_label = stable_label(current_name);
    if (cached_label.size() >= 2U && cached_label == current_label) {
        return 70;
    }
    return 0;
}

}  // namespace brimbar
