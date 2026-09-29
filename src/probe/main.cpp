#include "brimbar/tray_access.h"
#include "brimbar/native_tray.h"
#include "brimbar/tray_target.h"
#include "brimbar/version_policy.h"

#include <Windows.h>
#include <objbase.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

class com_runtime final {
public:
    com_runtime() noexcept : result_{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)} {}
    ~com_runtime() noexcept
    {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    com_runtime(const com_runtime&) = delete;
    com_runtime& operator=(const com_runtime&) = delete;
    com_runtime(com_runtime&&) = delete;
    com_runtime& operator=(com_runtime&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return SUCCEEDED(result_); }

private:
    HRESULT result_{};
};

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr));
    return result;
}

}  // namespace

int wmain(const int argument_count, wchar_t** arguments)
{
    const com_runtime runtime{};
    if (!runtime.ready()) {
        std::cerr << "COM initialization failed\n";
        return 2;
    }

    const auto include_overflow = argument_count > 1 && std::wstring_view{arguments[1]} == L"--all";
    const auto items = include_overflow
        ? brimbar::enumerate_all_tray_items()
        : brimbar::enumerate_tray_items();
    const auto native_snapshot = brimbar::enumerate_native_tray_items();
    std::cout << "kind\tautomation_id\tclass\tinvoke\tbounds\tsource_icon\tname\n";
    int invokable{};
    for (const auto& item : items) {
        if (item.invokable) {
            ++invokable;
        }
        std::cout << utf8(brimbar::tray_target_kind_name(item.kind)) << '\t'
                  << utf8(item.automation_id) << '\t' << utf8(item.class_name) << '\t'
                  << (item.invokable ? "yes" : "no") << '\t'
                  << item.source_bounds.left << ',' << item.source_bounds.top << ','
                  << item.source_bounds.right << ',' << item.source_bounds.bottom << '\t'
                  << utf8(item.source_icon_path) << '\t'
                  << utf8(item.name) << '\n';
    }

    const auto version = brimbar::current_windows_version();
    std::cout << "SUMMARY\tbuild=" << version.build
              << "\tcandidate=" << (brimbar::is_candidate_build(version.build) ? "yes" : "no")
              << "\ttargets=" << items.size()
              << "\tinvokable=" << invokable << '\n';
    std::cout << "NATIVE\tavailable=" << (native_snapshot.interface_available ? "yes" : "no")
              << "\thresult=0x" << std::hex << static_cast<unsigned long>(native_snapshot.result)
              << std::dec << "\tcallbacks=" << native_snapshot.callback_count
              << "\titems=" << native_snapshot.items.size() << '\n';
    for (const auto& item : native_snapshot.items) {
        std::cout << "NATIVE_ITEM\t" << item.identifier << '\t'
                  << item.preference << '\t' << (item.active_window ? "active" : "stale") << '\t'
                  << utf8(item.executable_path) << '\t'
                  << utf8(item.tooltip) << '\n';
    }
    return !items.empty() && invokable > 0 ? EXIT_SUCCESS : 5;
}
