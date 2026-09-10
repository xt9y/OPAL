#include <opal/display_backend.hpp>

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace opal {
namespace {

bool active_display(CGDirectDisplayID display_id = 0)
{
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) return false;
    if (display_id == 0) return true;
    std::vector<CGDirectDisplayID> displays(count);
    if (CGGetActiveDisplayList(count, displays.data(), &count) != kCGErrorSuccess) return false;
    return std::find(displays.begin(), displays.begin() + count, display_id) != displays.begin() + count;
}

id class_new(const char* name)
{
    Class cls = NSClassFromString([NSString stringWithUTF8String:name]);
    if (!cls) return nil;
    using Fn = id (*)(id, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(reinterpret_cast<id>(cls), sel_registerName("new"));
}

id class_alloc(const char* name)
{
    Class cls = NSClassFromString([NSString stringWithUTF8String:name]);
    if (!cls) return nil;
    using Fn = id (*)(id, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(reinterpret_cast<id>(cls), sel_registerName("alloc"));
}

bool responds(id object, const char* selector)
{
    return object && [object respondsToSelector:sel_registerName(selector)];
}

bool set_u32(id object, const char* selector, std::uint32_t value)
{
    if (!responds(object, selector)) return false;
    using Fn = void (*)(id, SEL, unsigned int);
    reinterpret_cast<Fn>(objc_msgSend)(object, sel_registerName(selector), static_cast<unsigned int>(value));
    return true;
}

bool set_object(id object, const char* selector, id value)
{
    if (!responds(object, selector)) return false;
    using Fn = void (*)(id, SEL, id);
    reinterpret_cast<Fn>(objc_msgSend)(object, sel_registerName(selector), value);
    return true;
}

bool set_size(id object, const char* selector, CGSize value)
{
    if (!responds(object, selector)) return false;
    using Fn = void (*)(id, SEL, CGSize);
    reinterpret_cast<Fn>(objc_msgSend)(object, sel_registerName(selector), value);
    return true;
}

bool set_queue(id object, dispatch_queue_t queue)
{
    if (!responds(object, "setDispatchQueue:")) return false;
    using Fn = void (*)(id, SEL, dispatch_queue_t);
    reinterpret_cast<Fn>(objc_msgSend)(object, sel_registerName("setDispatchQueue:"), queue);
    return true;
}

bool virtual_runtime_contract(id descriptor, id settings, id mode_alloc, id display_alloc)
{
    return descriptor && settings && mode_alloc && display_alloc &&
           responds(descriptor, "setVendorID:") &&
           responds(descriptor, "setProductID:") &&
           responds(descriptor, "setSerialNum:") &&
           responds(descriptor, "setName:") &&
           responds(descriptor, "setMaxPixelsWide:") &&
           responds(descriptor, "setMaxPixelsHigh:") &&
           responds(descriptor, "setSizeInMillimeters:") &&
           responds(descriptor, "setDispatchQueue:") &&
           responds(mode_alloc, "initWithWidth:height:refreshRate:") &&
           responds(settings, "setModes:") &&
           responds(settings, "setHiDPI:") &&
           responds(display_alloc, "initWithDescriptor:");
}

class MacDisplayBackend final : public DisplayBackend {
public:
    ~MacDisplayBackend() override { destroy_virtual(); }

    bool probe(DisplayTarget& target) override
    {
        error_ = {};
        if (!active_display()) return false;
        target = {};
        target.kind = DisplayKind::Physical;
        target.capture_kind = DisplayCaptureKind::Desktop;
        target.name = "macOS desktop";
        target.owned_by_opal = false;
        backend_ = "coregraphics-physical";
        return true;
    }

    bool ensure(const DisplayMode& requested, DisplayTarget& target) override
    {
        destroy_virtual();
        error_ = {};

        Class descriptor_class = NSClassFromString(@"CGVirtualDisplayDescriptor");
        Class display_class = NSClassFromString(@"CGVirtualDisplay");
        Class settings_class = NSClassFromString(@"CGVirtualDisplaySettings");
        Class mode_class = NSClassFromString(@"CGVirtualDisplayMode");
        if (!descriptor_class || !display_class || !settings_class || !mode_class) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "macOS CGVirtualDisplay runtime is unavailable", false};
            return false;
        }

        const int width = std::clamp(requested.width, 640, 7680);
        const int height = std::clamp(requested.height, 480, 4320);
        const int refresh = std::clamp(requested.refresh_hz, 30, 60);

        id descriptor = class_new("CGVirtualDisplayDescriptor");
        id settings = class_new("CGVirtualDisplaySettings");
        id mode_alloc = class_alloc("CGVirtualDisplayMode");
        id display_alloc = class_alloc("CGVirtualDisplay");
        if (!virtual_runtime_contract(descriptor, settings, mode_alloc, display_alloc)) {
            [descriptor release];
            [settings release];
            [mode_alloc release];
            [display_alloc release];
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "macOS CGVirtualDisplay runtime contract is incompatible", false};
            return false;
        }

        queue_ = dispatch_queue_create("de.xt9y.opal.virtual-display", DISPATCH_QUEUE_SERIAL);
        bool configured = queue_ && set_queue(descriptor, queue_);
        configured = configured && set_u32(descriptor, "setVendorID:", 0x4f50u);
        configured = configured && set_u32(descriptor, "setProductID:", 0x414cu);
        configured = configured && set_u32(descriptor, "setSerialNum:", 1u);
        configured = configured && set_object(descriptor, "setName:", @"OPAL Virtual Display");
        configured = configured && set_u32(descriptor, "setMaxPixelsWide:", 7680u);
        configured = configured && set_u32(descriptor, "setMaxPixelsHigh:", 4320u);
        configured = configured && set_size(descriptor, "setSizeInMillimeters:", CGSizeMake(597.0, 336.0));
        if (!configured) {
            [descriptor release];
            [settings release];
            [mode_alloc release];
            [display_alloc release];
            destroy_virtual();
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "macOS virtual display descriptor configuration is unsupported", false};
            return false;
        }

        using ModeInitFn = id (*)(id, SEL, NSUInteger, NSUInteger, CGFloat);
        id mode = reinterpret_cast<ModeInitFn>(objc_msgSend)(
            mode_alloc, sel_registerName("initWithWidth:height:refreshRate:"),
            static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), static_cast<CGFloat>(refresh));
        if (!mode) {
            [descriptor release];
            [settings release];
            [display_alloc release];
            destroy_virtual();
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "could not create macOS virtual display mode", false};
            return false;
        }

        const bool settings_ok = set_object(settings, "setModes:", @[mode]) &&
                                 set_u32(settings, "setHiDPI:", 0u);
        if (!settings_ok) {
            [mode release];
            [settings release];
            [descriptor release];
            [display_alloc release];
            destroy_virtual();
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "macOS virtual display settings runtime is incompatible", false};
            return false;
        }

        using DisplayInitFn = id (*)(id, SEL, id);
        virtual_display_ = reinterpret_cast<DisplayInitFn>(objc_msgSend)(
            display_alloc, sel_registerName("initWithDescriptor:"), descriptor);

        bool applied = false;
        if (virtual_display_ && responds(virtual_display_, "applySettings:")) {
            using ApplyFn = BOOL (*)(id, SEL, id);
            applied = reinterpret_cast<ApplyFn>(objc_msgSend)(
                virtual_display_, sel_registerName("applySettings:"), settings) != NO;
        }

        std::uint64_t display_id = 0;
        if (applied && responds(virtual_display_, "displayID")) {
            using DisplayIdFn = unsigned int (*)(id, SEL);
            display_id = static_cast<std::uint64_t>(reinterpret_cast<DisplayIdFn>(objc_msgSend)(
                virtual_display_, sel_registerName("displayID")));
        }

        [mode release];
        [settings release];
        [descriptor release];

        if (!applied || display_id == 0) {
            destroy_virtual();
            error_ = {PlatformComponent::Capture, PlatformFailure::OsError,
                      "macOS rejected the OPAL virtual display", false};
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!active_display(static_cast<CGDirectDisplayID>(display_id)) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (!active_display(static_cast<CGDirectDisplayID>(display_id))) {
            destroy_virtual();
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "macOS virtual display did not become active", false};
            return false;
        }

        target = {};
        target.kind = DisplayKind::VirtualExistingSession;
        target.capture_kind = DisplayCaptureKind::NativeDisplay;
        target.mode = {width, height, refresh, 1.0f};
        target.name = "OPAL Virtual Display";
        target.native_id = display_id;
        target.owned_by_opal = true;
        backend_ = "coregraphics-virtual";
        return true;
    }

    bool reconfigure(const DisplayMode& mode, DisplayTarget& target) override
    {
        if (!target.owned_by_opal) return true;
        DisplayTarget replacement;
        if (!ensure(mode, replacement)) return false;
        target = std::move(replacement);
        return true;
    }

    bool healthy(const DisplayTarget& target) override
    {
        if (!target.virtual_display()) return active_display();
        return target.native_id != 0 && active_display(static_cast<CGDirectDisplayID>(target.native_id));
    }

    void release(DisplayTarget& target) override
    {
        if (target.owned_by_opal) destroy_virtual();
        target = {};
        backend_ = "coregraphics";
    }

    std::string backend_name() const override { return backend_; }
    PlatformError last_platform_error() const override { return error_; }

private:
    void destroy_virtual()
    {
        if (virtual_display_) {
            [virtual_display_ release];
            virtual_display_ = nil;
        }
        if (queue_) {
            dispatch_release(queue_);
            queue_ = nullptr;
        }
    }

    id virtual_display_ = nil;
    dispatch_queue_t queue_ = nullptr;
    std::string backend_ = "coregraphics";
    PlatformError error_{};
};

}

std::unique_ptr<DisplayBackend> make_display_backend()
{
    return std::make_unique<MacDisplayBackend>();
}

}
