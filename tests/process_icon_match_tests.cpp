#include "brimbar/process_icon_match.h"

#include <cstdlib>
#include <iostream>

int main()
{
    if (brimbar::process_icon_key(L"企业微信: 用户") != L"wxwork" ||
        brimbar::process_icon_key(L" 微信") != L"weixin" ||
        brimbar::process_icon_key(L" G-Helper CPU: 80°C") != L"ghelper" ||
        brimbar::process_icon_key(L"Windows 安全中心 - 不需要执行操作") != L"securityhealthsystray" ||
        !brimbar::process_icon_key(L"蓝牙设备").empty()) {
        std::cerr << "Tray label to process icon mapping failed\n";
        return EXIT_FAILURE;
    }

    if (brimbar::tray_identity_match_score(
            L"G-Helper CPU: 62°C", L"C:\\Apps\\GHelper.exe",
            L"G-Helper CPU: 88°C", L"C:\\Apps\\GHelper.exe") != 100 ||
        brimbar::tray_identity_match_score(
            L"ChatGPT - 已连接", L"", L"ChatGPT - 正在同步", L"") != 90 ||
        brimbar::tray_identity_match_score(
            L"未知应用：空闲", L"", L"未知应用：忙碌", L"") != 70 ||
        brimbar::tray_identity_match_score(
            L"微信", L"", L"NVIDIA 设置", L"") != 0) {
        std::cerr << "Tray identity matching failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
