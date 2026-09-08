#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#include <cstring>
#include <string>

extern "C" char* SDLCALL opal_macos_get_clipboard_text(void)
{
    SDL_ClearError();
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        NSString* value = [pasteboard stringForType:NSPasteboardTypeString];
        if (!value) {
            char* empty = static_cast<char*>(SDL_malloc(1));
            if (!empty) {
                SDL_SetError("NSPasteboard clipboard allocation failed");
                return nullptr;
            }
            empty[0] = '\0';
            return empty;
        }
        const char* utf8 = [value UTF8String];
        if (!utf8) {
            SDL_SetError("NSPasteboard clipboard text is not valid UTF-8");
            return nullptr;
        }
        const std::size_t size = std::strlen(utf8);
        char* out = static_cast<char*>(SDL_malloc(size + 1));
        if (!out) {
            SDL_SetError("NSPasteboard clipboard allocation failed");
            return nullptr;
        }
        std::memcpy(out, utf8, size + 1);
        return out;
    }
}

extern "C" bool SDLCALL opal_macos_set_clipboard_text(const char* text)
{
    SDL_ClearError();
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        if (!text || !*text) return true;
        NSString* value = [NSString stringWithUTF8String:text];
        if (!value) {
            SDL_SetError("NSPasteboard clipboard text is not valid UTF-8");
            return false;
        }
        if (![pasteboard setString:value forType:NSPasteboardTypeString]) {
            SDL_SetError("NSPasteboard clipboard write failed");
            return false;
        }
        return true;
    }
}
