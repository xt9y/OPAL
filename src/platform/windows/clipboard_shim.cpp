#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool open_clipboard(HWND owner = nullptr)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(1);
    }
    return false;
}

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring out(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            out.data(), required) != required) return {};
    return out;
}

std::string wide_to_utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string out(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            out.data(), required, nullptr, nullptr) != required) return {};
    return out;
}

bool read_clipboard_text(HWND owner, std::string& text)
{
    if (!open_clipboard(owner)) return false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        text.clear();
        CloseClipboard();
        return true;
    }

    const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data));
    if (!wide) {
        CloseClipboard();
        return false;
    }
    const std::wstring_view view(wide);
    std::string utf8 = wide_to_utf8(view);
    const bool valid = view.empty() || !utf8.empty();
    GlobalUnlock(data);
    CloseClipboard();
    if (!valid) return false;
    text = std::move(utf8);
    return true;
}

char* sdl_copy(std::string_view text)
{
    char* out = static_cast<char*>(SDL_malloc(text.size() + 1));
    if (!out) return nullptr;
    if (!text.empty()) std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

class ClipboardMonitor {
public:
    ClipboardMonitor()
    {
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
        if (ready_) (void)WaitForSingleObject(ready_, 1000);
    }

    ~ClipboardMonitor()
    {
        HWND window = window_.load(std::memory_order_acquire);
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
        if (ready_) CloseHandle(ready_);
    }

    std::string cached_text()
    {
        if (!valid_.load(std::memory_order_acquire)) {
            std::string text;
            if (read_clipboard_text(nullptr, text)) store(std::move(text));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

    void note_local_write(std::string text)
    {
        store(std::move(text));
    }

    std::uint64_t generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

private:
    static constexpr wchar_t kClassName[] = L"OPALClipboardMonitorWindow";

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        ClipboardMonitor* self = reinterpret_cast<ClipboardMonitor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ClipboardMonitor*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_CLIPBOARDUPDATE) {
            self->refresh(window);
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void run()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = &ClipboardMonitor::window_proc;
        klass.hInstance = instance;
        klass.lpszClassName = kClassName;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            signal_ready();
            return;
        }

        HWND window = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, instance, this);
        if (!window) {
            signal_ready();
            return;
        }
        window_.store(window, std::memory_order_release);
        if (!AddClipboardFormatListener(window)) {
            window_.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            signal_ready();
            return;
        }

        refresh(window);
        signal_ready();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RemoveClipboardFormatListener(window);
        window_.store(nullptr, std::memory_order_release);
    }

    void signal_ready()
    {
        if (ready_) SetEvent(ready_);
    }

    void refresh(HWND owner)
    {
        std::string text;
        if (read_clipboard_text(owner, text)) store(std::move(text));
    }

    void store(std::string text)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            changed = !valid_.load(std::memory_order_relaxed) || text_ != text;
            text_ = std::move(text);
            valid_.store(true, std::memory_order_release);
        }
        if (changed) generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::thread thread_;
    HANDLE ready_ = nullptr;
    std::atomic<HWND> window_{nullptr};
    mutable std::mutex mutex_;
    std::string text_;
    std::atomic<bool> valid_{false};
    std::atomic<std::uint64_t> generation_{0};
};

ClipboardMonitor& monitor()
{
    static ClipboardMonitor instance;
    return instance;
}

}

extern "C" std::uint64_t opal_windows_clipboard_generation(void)
{
    return monitor().generation();
}

extern "C" char* SDLCALL opal_windows_get_clipboard_text(void)
{
    SDL_ClearError();
    const std::string text = monitor().cached_text();
    char* out = sdl_copy(text);
    if (!out) SDL_SetError("Windows clipboard allocation failed");
    return out;
}

extern "C" bool SDLCALL opal_windows_set_clipboard_text(const char* text)
{
    SDL_ClearError();
    const std::string_view utf8 = text ? std::string_view(text) : std::string_view{};
    const std::wstring wide = utf8_to_wide(utf8);
    if (!utf8.empty() && wide.empty()) {
        SDL_SetError("Windows clipboard text is not valid UTF-8");
        return false;
    }

    if (!open_clipboard()) {
        SDL_SetError("OpenClipboard failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!EmptyClipboard()) {
        const DWORD error = GetLastError();
        CloseClipboard();
        SDL_SetError("EmptyClipboard failed (%lu)", static_cast<unsigned long>(error));
        return false;
    }
    if (wide.empty()) {
        CloseClipboard();
        monitor().note_local_write({});
        return true;
    }

    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        SDL_SetError("GlobalAlloc clipboard failed");
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        SDL_SetError("GlobalLock clipboard write failed");
        return false;
    }
    std::memcpy(destination, wide.c_str(), bytes);
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        const DWORD error = GetLastError();
        GlobalFree(memory);
        CloseClipboard();
        SDL_SetError("SetClipboardData failed (%lu)", static_cast<unsigned long>(error));
        return false;
    }
    CloseClipboard();
    monitor().note_local_write(std::string(utf8));
    return true;
}
