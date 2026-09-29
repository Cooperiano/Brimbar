#include "brimbar/tray_access.h"
#include "brimbar/icon_processing.h"
#include "brimbar/process_icon_match.h"

#include <OleAuto.h>
#include <TlHelp32.h>
#include <Unknwn.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>

namespace {

using Microsoft::WRL::ComPtr;

class unique_handle final {
public:
    explicit unique_handle(const HANDLE value) noexcept : value_{value} {}
    ~unique_handle() noexcept
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&&) = delete;
    unique_handle& operator=(unique_handle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{};
};

struct process_image final {
    std::wstring executable_name{};
    std::wstring path{};
};

[[nodiscard]] std::wstring lowercase(const std::wstring_view value)
{
    std::wstring result{value};
    std::ranges::transform(result, result.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

[[nodiscard]] std::vector<process_image> enumerate_process_images()
{
    const unique_handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
    if (!snapshot.valid()) {
        return {};
    }

    std::vector<process_image> result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return result;
    }
    do {
        const unique_handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
        if (!process.valid()) {
            continue;
        }
        std::wstring path(32768U, L'\0');
        DWORD path_length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0U, path.data(), &path_length)) {
            continue;
        }
        path.resize(path_length);
        auto executable_name = lowercase(std::filesystem::path(path).stem().wstring());
        result.push_back(process_image{std::move(executable_name), std::move(path)});
    } while (Process32NextW(snapshot.get(), &entry));
    return result;
}

[[nodiscard]] std::wstring find_process_icon_path(
    const std::wstring_view tray_name,
    const std::vector<process_image>& processes)
{
    const auto key = brimbar::process_icon_key(tray_name);
    if (key.empty()) {
        return {};
    }
    const auto match = std::ranges::find_if(processes, [&key](const process_image& process) {
        return process.executable_name.find(key) != std::wstring::npos;
    });
    return match == processes.end() ? std::wstring{} : match->path;
}

[[nodiscard]] std::wstring read_bstr(
    IUIAutomationElement& element,
    HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*))
{
    BSTR value{};
    if (FAILED((element.*getter)(&value)) || value == nullptr) {
        return {};
    }
    const std::wstring result{value, SysStringLen(value)};
    SysFreeString(value);
    return result;
}

[[nodiscard]] ComPtr<IUIAutomationElement> find_taskbar(IUIAutomation& automation)
{
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.GetRootElement(&root))) {
        return {};
    }

    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(L"Shell_TrayWnd");
    if (value.bstrVal == nullptr) {
        return {};
    }
    ComPtr<IUIAutomationCondition> condition;
    const auto result = automation.CreatePropertyCondition(UIA_ClassNamePropertyId, value, &condition);
    VariantClear(&value);
    if (FAILED(result)) {
        return {};
    }

    ComPtr<IUIAutomationElement> taskbar;
    if (FAILED(root->FindFirst(TreeScope_Subtree, condition.Get(), &taskbar))) {
        return {};
    }
    return taskbar;
}

[[nodiscard]] ComPtr<IUIAutomationElementArray> all_descendants(
    IUIAutomation& automation,
    IUIAutomationElement& root)
{
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreateTrueCondition(&condition))) {
        return {};
    }
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(root.FindAll(TreeScope_Subtree, condition.Get(), &elements))) {
        return {};
    }
    return elements;
}

[[nodiscard]] bool is_invokable(IUIAutomationElement& element) noexcept
{
    ComPtr<IUnknown> pattern;
    return SUCCEEDED(element.GetCurrentPattern(UIA_InvokePatternId, &pattern)) && pattern != nullptr;
}

[[nodiscard]] bool matches(const brimbar::tray_item& item, IUIAutomationElement& element)
{
    return item.automation_id == read_bstr(element, &IUIAutomationElement::get_CurrentAutomationId) &&
        item.class_name == read_bstr(element, &IUIAutomationElement::get_CurrentClassName) &&
        item.name == read_bstr(element, &IUIAutomationElement::get_CurrentName);
}

[[nodiscard]] std::wstring_view system_identity_key(const brimbar::tray_item& item) noexcept
{
    const auto contains = [&item](const std::wstring_view text) {
        return item.name.find(text) != std::wstring::npos;
    };
    if (item.kind == brimbar::tray_target_kind::show_desktop) {
        return L"show-desktop";
    }
    if (contains(L"隐藏的图标")) {
        return L"overflow";
    }
    if (contains(L"输入指示") || contains(L"中文模式")) {
        return L"input";
    }
    if (contains(L"网络")) {
        return L"network";
    }
    if (contains(L"音量") || contains(L"扬声器")) {
        return L"volume";
    }
    if (contains(L"电源") || contains(L"电池")) {
        return L"power";
    }
    if (contains(L"时钟")) {
        return L"clock";
    }
    return {};
}

[[nodiscard]] int item_match_score(
    const brimbar::tray_item& cached,
    const brimbar::tray_item& current)
{
    if (cached.kind != current.kind || cached.automation_id != current.automation_id) {
        return 0;
    }
    const auto cached_system_key = system_identity_key(cached);
    if (!cached_system_key.empty() && cached_system_key == system_identity_key(current)) {
        return 110;
    }
    if (cached.class_name != current.class_name) {
        return 0;
    }
    return brimbar::tray_identity_match_score(
        cached.name,
        cached.source_icon_path,
        current.name,
        current.source_icon_path);
}

[[nodiscard]] const brimbar::tray_item* best_current_match(
    const brimbar::tray_item& cached,
    const std::vector<brimbar::tray_item>& current) noexcept
{
    const brimbar::tray_item* best{};
    int best_score{};
    for (const auto& candidate : current) {
        const auto score = item_match_score(cached, candidate);
        if (score > best_score) {
            best = &candidate;
            best_score = score;
        }
    }
    return best;
}

[[nodiscard]] bool invoke_exact_item(
    const brimbar::tray_item& item,
    const bool desktop_scope) noexcept
{
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        return false;
    }

    ComPtr<IUIAutomationElement> scope;
    if (desktop_scope) {
        if (FAILED(automation->GetRootElement(&scope))) {
            return false;
        }
    } else {
        scope = find_taskbar(*automation.Get());
        if (scope == nullptr) {
            return false;
        }
    }
    const auto elements = all_descendants(*automation.Get(), *scope.Get());
    if (elements == nullptr) {
        return false;
    }

    int length{};
    if (FAILED(elements->get_Length(&length))) {
        return false;
    }
    for (int index{}; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, &element)) || !matches(item, *element.Get())) {
            continue;
        }
        ComPtr<IUnknown> pattern;
        if (FAILED(element->GetCurrentPattern(UIA_InvokePatternId, &pattern)) || pattern == nullptr) {
            return false;
        }
        ComPtr<IUIAutomationInvokePattern> invoke_pattern;
        if (FAILED(pattern.As(&invoke_pattern))) {
            return false;
        }
        return SUCCEEDED(invoke_pattern->Invoke());
    }
    return false;
}

[[nodiscard]] bool send_mouse_activation(
    const brimbar::tray_item& item,
    const brimbar::tray_activation activation) noexcept
{
    const auto width = item.source_bounds.right - item.source_bounds.left;
    const auto height = item.source_bounds.bottom - item.source_bounds.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    POINT original{};
    if (!GetCursorPos(&original)) {
        return false;
    }
    const auto x = item.source_bounds.left + width / 2;
    const auto y = item.source_bounds.top + height / 2;
    if (!SetCursorPos(x, y)) {
        return false;
    }

    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[1].type = INPUT_MOUSE;
    if (activation == brimbar::tray_activation::context_menu) {
        inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    } else {
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    }
    const auto sent = SendInput(2U, inputs, sizeof(INPUT)) == 2U;
    if (sent) {
        Sleep(50U);
    }
    static_cast<void>(SetCursorPos(original.x, original.y));
    return sent;
}

void capture_icon(brimbar::tray_item& item) noexcept
{
    constexpr int icon_size{30};
    const auto source_width = item.source_bounds.right - item.source_bounds.left;
    const auto source_height = item.source_bounds.bottom - item.source_bounds.top;
    if (source_width < icon_size || source_height < icon_size) {
        return;
    }

    const auto source_x = item.source_bounds.left + (source_width - icon_size) / 2;
    const auto source_y = item.source_bounds.top + (source_height - icon_size) / 2;
    const auto screen = GetDC(nullptr);
    const auto memory = CreateCompatibleDC(screen);
    if (screen == nullptr || memory == nullptr) {
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
        return;
    }

    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = icon_size;
    information.bmiHeader.biHeight = -icon_size;
    information.bmiHeader.biPlanes = 1U;
    information.bmiHeader.biBitCount = 32U;
    information.bmiHeader.biCompression = BI_RGB;
    void* raw_pixels{};
    const auto bitmap = CreateDIBSection(screen, &information, DIB_RGB_COLORS, &raw_pixels, nullptr, 0U);
    if (bitmap == nullptr || raw_pixels == nullptr) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }

    const auto previous = SelectObject(memory, bitmap);
    const auto copied = BitBlt(memory, 0, 0, icon_size, icon_size, screen, source_x, source_y, SRCCOPY);
    if (copied) {
        const auto pixel_count = static_cast<std::size_t>(icon_size * icon_size);
        const auto* pixels = static_cast<const std::uint32_t*>(raw_pixels);
        item.icon_pixels.assign(pixels, pixels + pixel_count);

        const std::array<std::uint32_t, 4> corners{
            item.icon_pixels.front(),
            item.icon_pixels[static_cast<std::size_t>(icon_size - 1)],
            item.icon_pixels[pixel_count - static_cast<std::size_t>(icon_size)],
            item.icon_pixels.back(),
        };
        int background_blue{};
        int background_green{};
        int background_red{};
        for (const auto pixel : corners) {
            background_blue += static_cast<int>(pixel & 0xFFU);
            background_green += static_cast<int>((pixel >> 8U) & 0xFFU);
            background_red += static_cast<int>((pixel >> 16U) & 0xFFU);
        }
        background_blue /= static_cast<int>(corners.size());
        background_green /= static_cast<int>(corners.size());
        background_red /= static_cast<int>(corners.size());

        for (auto& pixel : item.icon_pixels) {
            const auto blue = static_cast<int>(pixel & 0xFFU);
            const auto green = static_cast<int>((pixel >> 8U) & 0xFFU);
            const auto red = static_cast<int>((pixel >> 16U) & 0xFFU);
            const auto difference =
                std::abs(blue - background_blue) +
                std::abs(green - background_green) +
                std::abs(red - background_red);
            const auto alpha = brimbar::icon_alpha_from_difference(difference);
            const auto premultiplied_blue = blue * alpha / 255;
            const auto premultiplied_green = green * alpha / 255;
            const auto premultiplied_red = red * alpha / 255;
            pixel = (static_cast<std::uint32_t>(alpha) << 24U) |
                (static_cast<std::uint32_t>(premultiplied_red) << 16U) |
                (static_cast<std::uint32_t>(premultiplied_green) << 8U) |
                static_cast<std::uint32_t>(premultiplied_blue);
        }
        item.icon_width = icon_size;
        item.icon_height = icon_size;
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

[[nodiscard]] std::vector<brimbar::tray_item> enumerate_overflow_items()
{
    const auto processes = enumerate_process_images();
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        return {};
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->GetRootElement(&root))) {
        return {};
    }
    const auto elements = all_descendants(*automation.Get(), *root.Get());
    if (elements == nullptr) {
        return {};
    }

    int length{};
    if (FAILED(elements->get_Length(&length))) {
        return {};
    }
    std::vector<brimbar::tray_item> result;
    for (int index{}; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, &element))) {
            continue;
        }
        const auto automation_id = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentAutomationId);
        if (automation_id != L"NotifyItemIcon") {
            continue;
        }
        brimbar::tray_item item{};
        item.kind = brimbar::tray_target_kind::notification_icon;
        item.automation_id = automation_id;
        item.class_name = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentClassName);
        item.name = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentName);
        static_cast<void>(element->get_CurrentBoundingRectangle(&item.source_bounds));
        if (item.source_bounds.bottom >= GetSystemMetrics(SM_CYSCREEN) - 80 || item.source_bounds.top <= 0) {
            continue;
        }
        item.invokable = is_invokable(*element.Get());
        item.overflow = true;
        item.source_icon_path = find_process_icon_path(item.name, processes);
        capture_icon(item);
        result.push_back(std::move(item));
    }
    return result;
}

}  // namespace

namespace brimbar {

std::vector<tray_item> enumerate_tray_items()
{
    const auto processes = enumerate_process_images();
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        return {};
    }
    const auto taskbar = find_taskbar(*automation.Get());
    if (taskbar == nullptr) {
        return {};
    }
    const auto elements = all_descendants(*automation.Get(), *taskbar.Get());
    if (elements == nullptr) {
        return {};
    }

    int length{};
    if (FAILED(elements->get_Length(&length))) {
        return {};
    }

    std::vector<tray_item> result;
    result.reserve(static_cast<std::size_t>(length));
    for (int index{}; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, &element))) {
            continue;
        }
        tray_item item{};
        item.automation_id = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentAutomationId);
        item.class_name = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentClassName);
        item.kind = classify_tray_target(item.automation_id, item.class_name);
        if (item.kind == tray_target_kind::unknown) {
            continue;
        }
        item.name = read_bstr(*element.Get(), &IUIAutomationElement::get_CurrentName);
        static_cast<void>(element->get_CurrentBoundingRectangle(&item.source_bounds));
        item.invokable = is_invokable(*element.Get());
        item.source_icon_path = find_process_icon_path(item.name, processes);
        if (item.kind == tray_target_kind::notification_icon) {
            capture_icon(item);
        }
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<tray_item> enumerate_available_tray_items()
{
    auto visible = enumerate_tray_items();
    auto overflow = enumerate_overflow_items();
    if (overflow.empty()) {
        return visible;
    }

    std::erase_if(visible, [&overflow](const tray_item& item) {
        if (item.name.find(L"隐藏的图标") != std::wstring::npos) {
            return true;
        }
        return item.kind == tray_target_kind::notification_icon &&
            best_current_match(item, overflow) != nullptr;
    });
    visible.insert(
        visible.begin(),
        std::make_move_iterator(overflow.begin()),
        std::make_move_iterator(overflow.end()));
    return visible;
}

std::vector<tray_item> enumerate_all_tray_items()
{
    auto visible = enumerate_tray_items();
    auto overflow = enumerate_overflow_items();
    const auto chevron = std::find_if(visible.begin(), visible.end(), [](const tray_item& item) {
        return item.kind == tray_target_kind::system_icon && item.name.find(L"隐藏的图标") != std::wstring::npos;
    });
    bool opened_flyout{};
    try {
        if (overflow.empty() && chevron != visible.end() && invoke_tray_item(*chevron)) {
            opened_flyout = true;
            Sleep(500U);
            overflow = enumerate_overflow_items();
            if (overflow.empty()) {
                Sleep(200U);
                overflow = enumerate_overflow_items();
            }
        }
    } catch (...) {
        if (opened_flyout) {
            static_cast<void>(invoke_tray_item(*chevron));
        }
        throw;
    }
    if (opened_flyout) {
        static_cast<void>(invoke_tray_item(*chevron));
    }

    if (chevron != visible.end()) {
        visible.erase(chevron);
    }
    std::erase_if(visible, [&overflow](const tray_item& item) {
        return item.kind == tray_target_kind::notification_icon &&
            best_current_match(item, overflow) != nullptr;
    });
    visible.insert(
        visible.begin(),
        std::make_move_iterator(overflow.begin()),
        std::make_move_iterator(overflow.end()));
    return visible;
}

bool invoke_tray_item(const tray_item& item) noexcept
{
    if (item.overflow) {
        try {
            const auto visible = enumerate_tray_items();
            auto overflow = enumerate_overflow_items();
            const auto chevron = std::find_if(visible.begin(), visible.end(), [](const tray_item& candidate) {
                return candidate.kind == tray_target_kind::system_icon &&
                    candidate.name.find(L"隐藏的图标") != std::wstring::npos;
            });
            if (overflow.empty() && chevron == visible.end()) {
                return false;
            }

            bool opened_flyout{};
            if (overflow.empty()) {
                if (!invoke_tray_item(*chevron)) {
                    return false;
                }
                opened_flyout = true;
                for (int attempt{}; attempt < 10 && overflow.empty(); ++attempt) {
                    Sleep(60U);
                    overflow = enumerate_overflow_items();
                }
            }

            const auto* target = best_current_match(item, overflow);
            if (target != nullptr && invoke_exact_item(*target, true)) {
                return true;
            }
            if (opened_flyout && chevron != visible.end()) {
                static_cast<void>(invoke_tray_item(*chevron));
            }
        } catch (...) {
            return false;
        }
        return false;
    }

    const auto current = enumerate_tray_items();
    const auto* target = best_current_match(item, current);
    return target != nullptr && invoke_exact_item(*target, false);
}

bool activate_tray_item(
    const tray_item& item,
    const tray_activation activation) noexcept
{
    try {
        if (!item.overflow) {
            const auto current = enumerate_tray_items();
            const auto* target = best_current_match(item, current);
            return target != nullptr && send_mouse_activation(*target, activation);
        }

        const auto visible = enumerate_tray_items();
        auto overflow = enumerate_overflow_items();
        const auto chevron = std::find_if(visible.begin(), visible.end(), [](const tray_item& candidate) {
            return candidate.kind == tray_target_kind::system_icon &&
                candidate.name.find(L"隐藏的图标") != std::wstring::npos;
        });
        if (overflow.empty() && chevron == visible.end()) {
            return false;
        }

        bool opened_flyout{};
        if (overflow.empty()) {
            if (!invoke_tray_item(*chevron)) {
                return false;
            }
            opened_flyout = true;
            for (int attempt{}; attempt < 10 && overflow.empty(); ++attempt) {
                Sleep(60U);
                overflow = enumerate_overflow_items();
            }
        }

        const auto* target = best_current_match(item, overflow);
        if (target != nullptr && send_mouse_activation(*target, activation)) {
            return true;
        }
        if (opened_flyout && chevron != visible.end()) {
            static_cast<void>(invoke_tray_item(*chevron));
        }
    } catch (...) {
        return false;
    }
    return false;
}

}  // namespace brimbar
