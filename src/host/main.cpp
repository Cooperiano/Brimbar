#include "appbar.h"
#include "brimbar/tray_access.h"

#include <Windows.h>
#include <objbase.h>
#include <CommCtrl.h>
#include <commoncontrols.h>
#include <Shellapi.h>
#include <Windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class_name[]{L"Brimbar.TopBar"};
constexpr wchar_t metrics_mapping_name[]{L"Local\\MenuBar.Metrics.v1"};
constexpr std::uint32_t metrics_magic{0x3154424DU};
constexpr std::uint32_t metrics_version{1U};
constexpr int logical_bar_height{28};
constexpr int logical_tray_icon_size{16};
constexpr UINT_PTR clock_timer_id{1U};
constexpr UINT tray_loaded_message{WM_APP + 43U};
constexpr UINT tray_invoked_message{WM_APP + 44U};
constexpr UINT refresh_command_id{1001U};
constexpr UINT exit_command_id{1002U};

struct shared_metrics final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::int64_t timestamp{};
    double cpu{};
    double gpu{};
    double memory{};
    double power{};
};

struct metric_values final {
    std::optional<double> cpu{};
    std::optional<double> gpu{};
    std::optional<double> memory{};
    std::optional<double> power{};
};

struct active_application_info final {
    std::wstring name{};
    std::wstring path{};
};

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

private:
    HRESULT result_{};
};

class gdiplus_runtime final {
public:
    gdiplus_runtime() noexcept
    {
        Gdiplus::GdiplusStartupInput input{};
        status_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }

    ~gdiplus_runtime() noexcept
    {
        if (status_ == Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    gdiplus_runtime(const gdiplus_runtime&) = delete;
    gdiplus_runtime& operator=(const gdiplus_runtime&) = delete;
    gdiplus_runtime(gdiplus_runtime&&) = delete;
    gdiplus_runtime& operator=(gdiplus_runtime&&) = delete;

    [[nodiscard]] bool available() const noexcept { return status_ == Gdiplus::Ok; }

private:
    ULONG_PTR token_{};
    Gdiplus::Status status_{Gdiplus::GenericError};
};

class icon_handle final {
public:
    icon_handle() = default;
    explicit icon_handle(const HICON value) noexcept : value_{value} {}
    ~icon_handle() noexcept
    {
        if (value_ != nullptr) {
            DestroyIcon(value_);
        }
    }

    icon_handle(const icon_handle&) = delete;
    icon_handle& operator=(const icon_handle&) = delete;
    icon_handle(icon_handle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
    icon_handle& operator=(icon_handle&& other) noexcept
    {
        if (this != &other) {
            if (value_ != nullptr) {
                DestroyIcon(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HICON get() const noexcept { return value_; }

private:
    HICON value_{};
};

struct window_state final {
    brimbar::appbar bar{};
    HFONT font{};
    HFONT metric_label_font{};
    HFONT metric_value_font{};
    std::vector<brimbar::tray_item> tray_items{};
    std::vector<icon_handle> source_icons{};
    std::vector<std::wstring> source_icon_paths{};
    std::vector<std::pair<RECT, std::size_t>> tray_hits{};
    std::optional<std::size_t> hot_item{};
    metric_values metrics{};
    std::wstring active_application{L"桌面"};
    std::wstring active_application_path{};
    icon_handle active_application_icon{};
    unsigned int refresh_tick{};
    bool tray_load_in_progress{};
    bool tray_invoke_in_progress{};
};

[[nodiscard]] metric_values read_metrics() noexcept
{
    metric_values result{};
    const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, metrics_mapping_name);
    if (mapping == nullptr) {
        return result;
    }
    const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0U, 0U, sizeof(shared_metrics));
    if (view == nullptr) {
        CloseHandle(mapping);
        return result;
    }

    shared_metrics snapshot{};
    std::memcpy(&snapshot, view, sizeof(snapshot));
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (snapshot.magic != metrics_magic || snapshot.version != metrics_version) {
        return result;
    }

    const auto valid_percent = [](const double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 100.0;
    };
    if (valid_percent(snapshot.cpu)) {
        result.cpu = snapshot.cpu;
    }
    if (valid_percent(snapshot.gpu)) {
        result.gpu = snapshot.gpu;
    }
    if (valid_percent(snapshot.memory)) {
        result.memory = snapshot.memory;
    }
    if (std::isfinite(snapshot.power) && snapshot.power >= 0.0 && snapshot.power < 10000.0) {
        result.power = snapshot.power;
    }
    return result;
}

[[nodiscard]] std::optional<active_application_info> read_active_application(
    const HWND own_window) noexcept
{
    const auto foreground = GetForegroundWindow();
    if (foreground == nullptr || foreground == own_window) {
        return {};
    }

    DWORD process_id{};
    GetWindowThreadProcessId(foreground, &process_id);
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return {};
    }

    std::array<wchar_t, 32768> path{};
    DWORD length{static_cast<DWORD>(path.size())};
    const auto found = QueryFullProcessImageNameW(process, 0U, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!found || length == 0U) {
        return {};
    }

    const std::wstring executable_path{path.data(), length};
    auto name = std::filesystem::path{executable_path}.stem().wstring();
    if (name == L"explorer") {
        name = L"文件资源管理器";
    }
    if (name == L"ApplicationFrameHost") {
        name = L"Windows 应用";
    }
    if (name.empty()) {
        return {};
    }
    return active_application_info{std::move(name), executable_path};
}

[[nodiscard]] std::wstring format_percent(const std::optional<double> value)
{
    return value ? std::format(L"{:.0f}%", *value) : L"--";
}

[[nodiscard]] std::wstring format_power(const std::optional<double> value)
{
    if (!value) {
        return L"--";
    }
    return *value < 10.0
        ? std::format(L"{:.1f}W", *value)
        : std::format(L"{:.0f}W", *value);
}

[[nodiscard]] icon_handle load_source_icon(const std::wstring& path) noexcept
{
    if (path.empty()) {
        return {};
    }

    SHFILEINFOW information{};
    if (SHGetFileInfoW(
            path.c_str(),
            0U,
            &information,
            sizeof(information),
            SHGFI_SYSICONINDEX) != 0U) {
        Microsoft::WRL::ComPtr<IImageList> image_list;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&image_list)))) {
            HICON icon{};
            if (SUCCEEDED(image_list->GetIcon(information.iIcon, ILD_TRANSPARENT, &icon))) {
                return icon_handle{icon};
            }
        }
    }

    if (SHGetFileInfoW(
            path.c_str(),
            0U,
            &information,
            sizeof(information),
            SHGFI_ICON | SHGFI_LARGEICON) != 0U) {
        return icon_handle{information.hIcon};
    }
    return {};
}

void rebuild_source_icons(window_state& state)
{
    std::vector<icon_handle> icons;
    std::vector<std::wstring> paths;
    icons.reserve(state.tray_items.size());
    paths.reserve(state.tray_items.size());
    for (const auto& item : state.tray_items) {
        icons.push_back(load_source_icon(item.source_icon_path));
        paths.push_back(item.source_icon_path);
    }
    state.source_icons = std::move(icons);
    state.source_icon_paths = std::move(paths);
}

[[nodiscard]] bool source_icon_cache_matches(const window_state& state) noexcept
{
    if (state.source_icon_paths.size() != state.tray_items.size()) {
        return false;
    }
    for (std::size_t index{}; index < state.tray_items.size(); ++index) {
        if (state.source_icon_paths[index] != state.tray_items[index].source_icon_path) {
            return false;
        }
    }
    return true;
}

struct tray_load_context final {
    HWND window{};
};

void CALLBACK load_tray_async(PTP_CALLBACK_INSTANCE, void* raw_context) noexcept
{
    const std::unique_ptr<tray_load_context> context{static_cast<tray_load_context*>(raw_context)};
    const com_runtime com{};
    auto items = std::make_unique<std::vector<brimbar::tray_item>>();
    try {
        *items = brimbar::enumerate_all_tray_items();
    } catch (...) {
        items->clear();
    }
    if (!PostMessageW(
            context->window,
            tray_loaded_message,
            0U,
            reinterpret_cast<LPARAM>(items.get()))) {
        return;
    }
    static_cast<void>(items.release());
}

[[nodiscard]] bool start_tray_load(const HWND window) noexcept
{
    auto context = std::make_unique<tray_load_context>();
    context->window = window;
    if (TrySubmitThreadpoolCallback(load_tray_async, context.get(), nullptr)) {
        static_cast<void>(context.release());
        return true;
    }
    return false;
}

struct tray_invoke_context final {
    HWND window{};
    brimbar::tray_item item{};
    brimbar::tray_activation activation{brimbar::tray_activation::primary};
};

void CALLBACK invoke_tray_async(PTP_CALLBACK_INSTANCE, void* raw_context) noexcept
{
    const std::unique_ptr<tray_invoke_context> context{
        static_cast<tray_invoke_context*>(raw_context)};
    const com_runtime com{};
    const auto invoked = brimbar::activate_tray_item(context->item, context->activation);
    static_cast<void>(PostMessageW(
        context->window,
        tray_invoked_message,
        invoked ? 1U : 0U,
        0));
}

[[nodiscard]] bool start_tray_invoke(
    const HWND window,
    const brimbar::tray_item& item,
    const brimbar::tray_activation activation) noexcept
{
    auto context = std::make_unique<tray_invoke_context>();
    context->window = window;
    context->item = item;
    context->activation = activation;
    if (TrySubmitThreadpoolCallback(invoke_tray_async, context.get(), nullptr)) {
        static_cast<void>(context.release());
        return true;
    }
    return false;
}

[[nodiscard]] int scaled_bar_height(const HWND window) noexcept
{
    return MulDiv(logical_bar_height, static_cast<int>(GetDpiForWindow(window)), 96);
}

[[nodiscard]] int scaled_tray_icon_size(const HWND window) noexcept
{
    return MulDiv(logical_tray_icon_size, static_cast<int>(GetDpiForWindow(window)), 96);
}

[[nodiscard]] HFONT create_ui_font(const HWND window) noexcept
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    const auto dpi = GetDpiForWindow(window);
    if (!SystemParametersInfoForDpi(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0U,
            dpi)) {
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0U)) {
            return nullptr;
        }
    }
    metrics.lfMessageFont.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

[[nodiscard]] HFONT create_metric_font(
    const HWND window,
    const int point_size,
    const int weight) noexcept
{
    return CreateFontW(
        -MulDiv(point_size, static_cast<int>(GetDpiForWindow(window)), 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

[[nodiscard]] std::wstring compact_label(const brimbar::tray_item& item)
{
    const auto contains = [&item](const std::wstring_view text) {
        return item.name.find(text) != std::wstring::npos;
    };
    if (item.kind == brimbar::tray_target_kind::show_desktop) {
        return L"桌面";
    }
    if (contains(L"隐藏的图标")) {
        return L"⌃";
    }
    if (contains(L"输入指示") || contains(L"中文模式")) {
        return L"中";
    }
    if (contains(L"网络")) {
        return L"网络";
    }
    if (contains(L"音量")) {
        return contains(L"静音") ? L"静音" : L"音量";
    }
    if (contains(L"电源") || contains(L"电池")) {
        const auto percent = item.name.find(L'%');
        if (percent != std::wstring::npos) {
            auto start = percent;
            while (start > 0U && item.name[start - 1U] >= L'0' && item.name[start - 1U] <= L'9') {
                --start;
            }
            return item.name.substr(start, percent - start + 1U);
        }
        return L"电池";
    }
    if (contains(L"时钟")) {
        const auto colon = item.name.find(L':');
        if (colon != std::wstring::npos && colon >= 2U && colon + 2U < item.name.size()) {
            return item.name.substr(colon - 2U, 5U);
        }
        return L"时间";
    }
    if (item.kind == brimbar::tray_target_kind::notification_icon) {
        auto start = item.name.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos) {
            return L"•";
        }
        const auto end = item.name.find_first_of(L" -\t\r\n", start);
        return item.overflow
            ? item.name.substr(start, 1U)
            : item.name.substr(start, std::min<std::size_t>(12U, end - start));
    }
    return L"状态";
}

void refresh_tray(const HWND window, window_state& state, const bool include_overflow) noexcept
{
    try {
        auto items = include_overflow
            ? brimbar::enumerate_all_tray_items()
            : brimbar::enumerate_available_tray_items();
        if (!include_overflow) {
            std::vector<brimbar::tray_item> merged;
            const auto has_current_overflow = std::ranges::any_of(
                items,
                [](const brimbar::tray_item& item) { return item.overflow; });
            if (!has_current_overflow) {
                std::copy_if(
                    state.tray_items.begin(), state.tray_items.end(), std::back_inserter(merged),
                    [](const brimbar::tray_item& item) { return item.overflow; });
            }
            std::copy_if(
                std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()),
                std::back_inserter(merged),
                [](const brimbar::tray_item& item) {
                    return item.name.find(L"隐藏的图标") == std::wstring::npos;
                });
            items = std::move(merged);
        }
        if (!items.empty()) {
            state.tray_items = std::move(items);
            if (!source_icon_cache_matches(state)) {
                rebuild_source_icons(state);
            }
        }
        const auto title = std::format(L"Menu Bar - {} items", state.tray_items.size());
        SetWindowTextW(window, title.c_str());
    } catch (...) {
        // Explorer/UIA failures must leave the last known-good tray snapshot intact.
    }
}

void paint_bar(const HWND window, window_state& state) noexcept
{
    PAINTSTRUCT paint{};
    const auto paint_dc = BeginPaint(window, &paint);
    if (paint_dc == nullptr) {
        return;
    }

    RECT bounds{};
    GetClientRect(window, &bounds);
    const auto buffer_width = bounds.right - bounds.left;
    const auto buffer_height = bounds.bottom - bounds.top;
    const auto buffer_dc = CreateCompatibleDC(paint_dc);
    const auto buffer_bitmap = CreateCompatibleBitmap(paint_dc, buffer_width, buffer_height);
    if (buffer_dc == nullptr || buffer_bitmap == nullptr) {
        if (buffer_bitmap != nullptr) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer_dc != nullptr) {
            DeleteDC(buffer_dc);
        }
        EndPaint(window, &paint);
        return;
    }
    const auto previous_bitmap = SelectObject(buffer_dc, buffer_bitmap);
    const auto dc = buffer_dc;

    const auto background = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    const auto top_highlight = CreateSolidBrush(RGB(55, 55, 61));
    RECT highlight{bounds.left, bounds.top, bounds.right, bounds.top + 1};
    FillRect(dc, &highlight, top_highlight);
    DeleteObject(top_highlight);

    const auto previous_font = SelectObject(dc, state.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(245, 245, 247));

    state.tray_hits.clear();
    const auto hover_brush = CreateSolidBrush(RGB(65, 65, 72));
    const auto hover_pen = CreatePen(PS_SOLID, 1, RGB(86, 86, 94));
    const auto previous_brush = SelectObject(dc, hover_brush);
    const auto previous_pen = SelectObject(dc, hover_pen);

    const auto target_icon_size = scaled_tray_icon_size(window);

    const auto configure_icon_graphics = [](Gdiplus::Graphics& graphics) {
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    };

    auto draw_icon_pixels = [&](brimbar::tray_item& item, const RECT item_bounds) {
        if (item.icon_pixels.empty() || item.icon_width <= 0 || item.icon_height <= 0) {
            return false;
        }
        Gdiplus::Bitmap bitmap{
            item.icon_width,
            item.icon_height,
            item.icon_width * static_cast<int>(sizeof(std::uint32_t)),
            PixelFormat32bppPARGB,
            reinterpret_cast<BYTE*>(item.icon_pixels.data())};
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }
        Gdiplus::Graphics graphics{dc};
        configure_icon_graphics(graphics);
        const auto target_x = item_bounds.left +
            (item_bounds.right - item_bounds.left - target_icon_size) / 2;
        const auto target_y = item_bounds.top +
            (item_bounds.bottom - item_bounds.top - target_icon_size) / 2;
        const Gdiplus::Rect target{target_x, target_y, target_icon_size, target_icon_size};
        return graphics.DrawImage(
            &bitmap,
            target,
            0,
            0,
            item.icon_width,
            item.icon_height,
            Gdiplus::UnitPixel) == Gdiplus::Ok;
    };

    auto draw_source_icon = [&](const HICON icon, const RECT item_bounds) {
        std::unique_ptr<Gdiplus::Bitmap> bitmap{Gdiplus::Bitmap::FromHICON(icon)};
        if (bitmap == nullptr || bitmap->GetLastStatus() != Gdiplus::Ok) {
            return false;
        }
        Gdiplus::Graphics graphics{dc};
        configure_icon_graphics(graphics);
        const auto target_x = item_bounds.left +
            (item_bounds.right - item_bounds.left - target_icon_size) / 2;
        const auto target_y = item_bounds.top +
            (item_bounds.bottom - item_bounds.top - target_icon_size) / 2;
        const Gdiplus::Rect target{target_x, target_y, target_icon_size, target_icon_size};
        return graphics.DrawImage(
            bitmap.get(),
            target,
            0,
            0,
            static_cast<int>(bitmap->GetWidth()),
            static_cast<int>(bitmap->GetHeight()),
            Gdiplus::UnitPixel) == Gdiplus::Ok;
    };

    auto draw_item = [&](const std::size_t index, const RECT item_bounds) {
        if (state.hot_item == index) {
            RoundRect(dc, item_bounds.left, item_bounds.top, item_bounds.right, item_bounds.bottom, 14, 14);
        }
        const auto source_icon = index < state.source_icons.size() &&
                index < state.source_icon_paths.size() &&
                state.source_icon_paths[index] == state.tray_items[index].source_icon_path
            ? state.source_icons[index].get()
            : nullptr;
        bool source_drawn{};
        if (source_icon != nullptr) {
            source_drawn = draw_source_icon(source_icon, item_bounds);
        }
        if (!source_drawn && !draw_icon_pixels(state.tray_items[index], item_bounds)) {
            auto label = compact_label(state.tray_items[index]);
            RECT text_rect{item_bounds.left + 5, item_bounds.top, item_bounds.right - 5, item_bounds.bottom};
            DrawTextW(dc, label.c_str(), -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        state.tray_hits.emplace_back(item_bounds, index);
    };

    auto right = bounds.right - 10;
    for (auto index = state.tray_items.size(); index-- > 0U;) {
        if (state.tray_items[index].overflow) {
            continue;
        }
        const auto label = compact_label(state.tray_items[index]);
        SIZE text_size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &text_size);
        const auto width = std::clamp(static_cast<int>(text_size.cx) + 18, 34, 92);
        RECT item_bounds{right - width, 4, right, bounds.bottom - 4};
        draw_item(index, item_bounds);
        right -= width + 3;
    }

    auto draw_separator = [&] {
        right -= 7;
        const auto separator_brush = CreateSolidBrush(RGB(62, 62, 68));
        RECT separator{right, 9, right + 1, bounds.bottom - 9};
        FillRect(dc, &separator, separator_brush);
        DeleteObject(separator_brush);
        right -= 7;
    };

    auto draw_metric = [&](const std::wstring_view label,
                           const std::wstring& value,
                           const COLORREF accent,
                           const int width) {
        RECT metric_bounds{right - width, 1, right, bounds.bottom - 1};
        const auto middle = metric_bounds.top + (metric_bounds.bottom - metric_bounds.top) / 2;
        RECT label_bounds{metric_bounds.left, metric_bounds.top, metric_bounds.right, middle + 1};
        SelectObject(dc, state.metric_label_font);
        SetTextColor(dc, RGB(218, 218, 223));
        DrawTextW(dc, label.data(), static_cast<int>(label.size()), &label_bounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT value_bounds{metric_bounds.left, middle - 1, metric_bounds.right, metric_bounds.bottom};
        SelectObject(dc, state.metric_value_font);
        SetTextColor(dc, accent);
        DrawTextW(dc, value.c_str(), -1, &value_bounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, state.font);
        right -= width + 2;
    };

    draw_separator();
    constexpr int minimum_application_width{190};
    constexpr int reserved_metrics_width{214};
    for (auto index = state.tray_items.size(); index-- > 0U;) {
        if (!state.tray_items[index].overflow ||
            right - 34 < minimum_application_width + reserved_metrics_width) {
            continue;
        }
        RECT item_bounds{right - 34, 4, right, bounds.bottom - 4};
        draw_item(index, item_bounds);
        right -= 36;
    }

    draw_separator();
    draw_metric(L"PWR", format_power(state.metrics.power), RGB(255, 141, 161), 56);
    draw_metric(L"RAM", format_percent(state.metrics.memory), RGB(255, 197, 104), 50);
    draw_metric(L"GPU", format_percent(state.metrics.gpu), RGB(114, 230, 174), 50);
    draw_metric(L"CPU", format_percent(state.metrics.cpu), RGB(101, 213, 255), 50);

    auto application_text_left = 10;
    if (state.active_application_icon.get() != nullptr) {
        const RECT application_icon_bounds{10, 3, 38, bounds.bottom - 3};
        static_cast<void>(draw_source_icon(state.active_application_icon.get(), application_icon_bounds));
        application_text_left = 43;
    }
    RECT application_bounds{
        application_text_left,
        0,
        std::max<LONG>(application_text_left, right - 12),
        bounds.bottom};
    SetTextColor(dc, RGB(245, 245, 247));
    DrawTextW(dc, state.active_application.c_str(), -1, &application_bounds,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (state.tray_items.empty()) {
        RECT loading{std::max<LONG>(20, right), 0, bounds.right - 20, bounds.bottom};
        SetTextColor(dc, RGB(180, 180, 188));
        DrawTextW(dc, L"正在接入系统托盘…", -1, &loading, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
    DeleteObject(hover_pen);
    DeleteObject(hover_brush);

    SelectObject(dc, previous_font);
    BitBlt(
        paint_dc,
        bounds.left,
        bounds.top,
        buffer_width,
        buffer_height,
        buffer_dc,
        0,
        0,
        SRCCOPY);
    SelectObject(buffer_dc, previous_bitmap);
    DeleteObject(buffer_bitmap);
    DeleteDC(buffer_dc);
    EndPaint(window, &paint);
}

[[nodiscard]] std::optional<std::size_t> hit_test_tray(
    const window_state& state,
    const POINT point) noexcept
{
    for (const auto& [bounds, index] : state.tray_hits) {
        if (PtInRect(&bounds, point)) {
            return index;
        }
    }
    return {};
}

void show_background_menu(const HWND window, const POINT screen_point) noexcept
{
    const auto menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, refresh_command_id, L"刷新托盘");
    AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
    AppendMenuW(menu, MF_STRING, exit_command_id, L"退出顶栏");
    SetForegroundWindow(window);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, screen_point.x, screen_point.y, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM w_param, const LPARAM l_param)
{
    auto* state = reinterpret_cast<window_state*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        state = static_cast<window_state*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        state->font = create_ui_font(window);
        state->metric_label_font = create_metric_font(window, 6, FW_LIGHT);
        state->metric_value_font = create_metric_font(window, 9, FW_NORMAL);
        if (!state->bar.attach(window, scaled_bar_height(window))) {
            return -1;
        }
        state->tray_load_in_progress = start_tray_load(window);
        SetTimer(window, clock_timer_id, 1000U, nullptr);
        return 0;
    }
    case WM_TIMER:
        state->metrics = read_metrics();
        if (auto active = read_active_application(window)) {
            state->active_application = std::move(active->name);
            if (active->path != state->active_application_path) {
                state->active_application_path = std::move(active->path);
                state->active_application_icon = load_source_icon(state->active_application_path);
            }
        }
        if (!state->tray_load_in_progress && ++state->refresh_tick % 2U == 0U) {
            refresh_tray(window, *state, false);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        // The complete frame is composed off-screen and committed atomically.
        return 1;
    case WM_PAINT:
        paint_bar(window, *state);
        return 0;
    case tray_loaded_message: {
        const std::unique_ptr<std::vector<brimbar::tray_item>> items{
            reinterpret_cast<std::vector<brimbar::tray_item>*>(l_param)};
        state->tray_load_in_progress = false;
        if (items != nullptr && !items->empty()) {
            state->tray_items = std::move(*items);
            rebuild_source_icons(*state);
            const auto title = std::format(L"Menu Bar - {} items", state->tray_items.size());
            SetWindowTextW(window, title.c_str());
            InvalidateRect(window, nullptr, FALSE);
        } else {
            refresh_tray(window, *state, false);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const auto index = hit_test_tray(*state, point);
        if (index && *index < state->tray_items.size() && !state->tray_invoke_in_progress) {
            state->tray_invoke_in_progress = start_tray_invoke(
                window,
                state->tray_items[*index],
                brimbar::tray_activation::primary);
        }
        return 0;
    }
    case tray_invoked_message:
        state->tray_invoke_in_progress = false;
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const auto hot = hit_test_tray(*state, point);
        if (hot != state->hot_item) {
            state->hot_item = hot;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0U};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hot_item.reset();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CONTEXTMENU: {
        POINT screen_point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (screen_point.x == -1 && screen_point.y == -1) {
            GetCursorPos(&screen_point);
        }
        auto client_point = screen_point;
        ScreenToClient(window, &client_point);
        const auto index = hit_test_tray(*state, client_point);
        if (index && *index < state->tray_items.size() && !state->tray_invoke_in_progress) {
            state->tray_invoke_in_progress = start_tray_invoke(
                window,
                state->tray_items[*index],
                brimbar::tray_activation::context_menu);
        } else if (!index) {
            show_background_menu(window, screen_point);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w_param) == refresh_command_id) {
            if (!state->tray_load_in_progress) {
                state->tray_load_in_progress = start_tray_load(window);
            }
            return 0;
        }
        if (LOWORD(w_param) == exit_command_id) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        state->bar.detach();
        if (state->font != nullptr) {
            DeleteObject(state->font);
        }
        if (state->metric_label_font != nullptr) {
            DeleteObject(state->metric_label_font);
        }
        if (state->metric_value_font != nullptr) {
            DeleteObject(state->metric_value_font);
        }
        state->font = create_ui_font(window);
        state->metric_label_font = create_metric_font(window, 6, FW_LIGHT);
        state->metric_value_font = create_metric_font(window, 9, FW_NORMAL);
        if (!state->bar.attach(window, scaled_bar_height(window))) {
            DestroyWindow(window);
        }
        return 0;
    case brimbar::appbar::callback_message:
        if (w_param == ABN_POSCHANGED) {
            state->bar.reposition();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, clock_timer_id);
        state->bar.detach();
        if (state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        if (state->metric_label_font != nullptr) {
            DeleteObject(state->metric_label_font);
            state->metric_label_font = nullptr;
        }
        if (state->metric_value_font != nullptr) {
            DeleteObject(state->metric_value_font);
            state->metric_value_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const com_runtime com{};
    const gdiplus_runtime gdiplus{};
    if (!gdiplus.available()) {
        return 3;
    }
    const auto mutex = CreateMutexW(nullptr, TRUE, L"Local\\Brimbar.TopBar");
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex != nullptr) {
            CloseHandle(mutex);
        }
        return 0;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = window_class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassExW(&window_class) == 0) {
        CloseHandle(mutex);
        return 1;
    }

    window_state state{};
    const auto window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        window_class_name,
        L"Menu Bar",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        CloseHandle(mutex);
        return 2;
    }

    constexpr DWORD dark_mode{1U};
    DwmSetWindowAttribute(window, 20, &dark_mode, sizeof(dark_mode));
    ShowWindow(window, SW_SHOWNOACTIVATE);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
