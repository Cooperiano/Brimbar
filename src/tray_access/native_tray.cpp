#include "brimbar/native_tray.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <utility>

namespace {

struct notify_item final {
    PWSTR executable_path;
    PWSTR tooltip;
    HICON icon;
    HWND window;
    DWORD preference;
    UINT identifier;
    GUID guid;
};

class __declspec(uuid("D782CCBA-AFB0-43F1-94DB-FDA3779EACCB")) notification_callback
    : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, notify_item* item) = 0;
};

class __declspec(uuid("FB852B2C-6BAD-4605-9551-F15F87830935")) tray_notify_legacy
    : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(notification_callback* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const notify_item* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enabled) = 0;
};

class __declspec(uuid("D133CE13-3537-48BA-93A7-AFCD5D2053B4")) tray_notify_win8
    : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(
        notification_callback* callback,
        unsigned long* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(unsigned long* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const notify_item* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL action) = 0;
};

constexpr CLSID tray_notify_class{
    0x25DEAD04,
    0x1EAC,
    0x4911,
    {0x9E, 0x3A, 0xAD, 0x0A, 0x4A, 0xB5, 0x60, 0xFD}};

[[nodiscard]] std::vector<std::uint32_t> copy_icon_pixels(
    const HICON icon,
    const int size) noexcept
{
    if (icon == nullptr || size <= 0) {
        return {};
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = size;
    bitmap_info.bmiHeader.biHeight = -size;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return {};
    }
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateDIBSection(
        screen, &bitmap_info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (memory == nullptr || bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        return {};
    }

    const HGDIOBJ previous = SelectObject(memory, bitmap);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size * size));
    if (DrawIconEx(memory, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL) != FALSE) {
        const auto* source = static_cast<const std::uint32_t*>(bits);
        std::copy_n(source, pixels.size(), pixels.begin());
    } else {
        pixels.clear();
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return pixels;
}

class callback_sink final : public notification_callback {
public:
    callback_sink(
        std::vector<brimbar::native_tray_item>& destination,
        std::uint32_t& callback_count) noexcept
        : destination_{destination}, callback_count_{callback_count}
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(const IID& iid, void** value) override
    {
        if (value == nullptr) {
            return E_POINTER;
        }
        *value = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(notification_callback)) {
            *value = static_cast<notification_callback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references_;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Notify(const ULONG event, notify_item* item) override
    {
        ++callback_count_;
        if (item == nullptr) {
            return S_OK;
        }

        brimbar::native_tray_item copy{};
        if (item->executable_path != nullptr) {
            copy.executable_path = item->executable_path;
        }
        if (item->tooltip != nullptr) {
            copy.tooltip = item->tooltip;
        }
        copy.owner_window = reinterpret_cast<std::uintptr_t>(item->window);
        copy.identifier = item->identifier;
        copy.guid = item->guid;
        copy.preference = item->preference;
        copy.event = event;
        copy.active_window = item->window != nullptr && IsWindow(item->window) != FALSE;
        constexpr int icon_size{32};
        copy.icon_pixels = copy_icon_pixels(item->icon, icon_size);
        if (!copy.icon_pixels.empty()) {
            copy.icon_width = icon_size;
            copy.icon_height = icon_size;
        }
        destination_.push_back(std::move(copy));
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1U};
    std::vector<brimbar::native_tray_item>& destination_;
    std::uint32_t& callback_count_;
};

}  // namespace

brimbar::native_tray_snapshot brimbar::enumerate_native_tray_items() noexcept
{
    native_tray_snapshot snapshot{};
    IUnknown* unknown{};
    snapshot.result = CoCreateInstance(
        tray_notify_class,
        nullptr,
        CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
        IID_IUnknown,
        reinterpret_cast<void**>(&unknown));
    if (FAILED(snapshot.result) || unknown == nullptr) {
        return snapshot;
    }

    auto* callback = new (std::nothrow) callback_sink{snapshot.items, snapshot.callback_count};
    if (callback == nullptr) {
        unknown->Release();
        snapshot.result = E_OUTOFMEMORY;
        return snapshot;
    }

    tray_notify_win8* modern{};
    snapshot.result = unknown->QueryInterface(
        __uuidof(tray_notify_win8), reinterpret_cast<void**>(&modern));
    if (SUCCEEDED(snapshot.result) && modern != nullptr) {
        snapshot.interface_available = true;
        unsigned long cookie{};
        snapshot.result = modern->RegisterCallback(callback, &cookie);
        if (SUCCEEDED(snapshot.result)) {
            static_cast<void>(modern->UnregisterCallback(&cookie));
        }
        modern->Release();
    } else {
        tray_notify_legacy* legacy{};
        snapshot.result = unknown->QueryInterface(
            __uuidof(tray_notify_legacy), reinterpret_cast<void**>(&legacy));
        if (SUCCEEDED(snapshot.result) && legacy != nullptr) {
            snapshot.interface_available = true;
            snapshot.result = legacy->RegisterCallback(callback);
            static_cast<void>(legacy->RegisterCallback(nullptr));
            legacy->Release();
        }
    }

    callback->Release();
    unknown->Release();
    return snapshot;
}
