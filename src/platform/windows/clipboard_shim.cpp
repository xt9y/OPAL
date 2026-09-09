#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>

namespace {

bool open_clipboard()
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
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

char* sdl_copy(std::string_view text)
{
    char* out = static_cast<char*>(SDL_malloc(text.size() + 1));
    if (!out) return nullptr;
    if (!text.empty()) std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

}

extern "C" char* SDLCALL opal_windows_get_clipboard_text(void)
{
    SDL_ClearError();
    if (!open_clipboard()) {
        SDL_SetError("OpenClipboard failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return sdl_copy({});
    }

    const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(data));
    if (!wide) {
        const DWORD error = GetLastError();
        CloseClipboard();
        SDL_SetError("GlobalLock clipboard failed (%lu)", static_cast<unsigned long>(error));
        return nullptr;
    }

    const std::wstring_view view(wide);
    const std::string utf8 = wide_to_utf8(view);
    GlobalUnlock(data);
    CloseClipboard();
    if (!view.empty() && utf8.empty()) {
        SDL_SetError("Windows clipboard text is not valid Unicode");
        return nullptr;
    }
    char* out = sdl_copy(utf8);
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
    return true;
}
