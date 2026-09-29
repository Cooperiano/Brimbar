#include "brimbar/version_policy.h"

#include <Windows.h>
#include <winternl.h>

namespace brimbar {
namespace {

using rtl_get_version_fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

}  // namespace

windows_version current_windows_version() noexcept
{
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return {};
    }

    const auto rtl_get_version = reinterpret_cast<rtl_get_version_fn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        return {};
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) != 0) {
        return {};
    }

    return {
        .major = info.dwMajorVersion,
        .minor = info.dwMinorVersion,
        .build = info.dwBuildNumber,
    };
}

}  // namespace brimbar
