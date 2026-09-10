#ifdef __linux__
#include <opal/input_record.hpp>

#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-client.h>

#include "fake-input-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/ioctl.h>
#include <vector>

namespace {
constexpr int pointer_max = 65535;

bool env_enabled(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value && std::string_view(value) != "0";
}

int env_dimension(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!end || *end || parsed < 1 || parsed > INT_MAX) return fallback;
    return static_cast<int>(parsed);
}

int button_code(int button)
{
    return button == 1 ? BTN_LEFT : button == 2 ? BTN_MIDDLE : button == 3 ? BTN_RIGHT : 0;
}

struct OutputState {
    wl_output* output = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int scale = 1;
    bool current = false;
};

class KwinInput {
public:
    ~KwinInput() { stop(); }

    bool start()
    {
        stop();
        if (!env_enabled("OPAL_KWIN_VIRTUAL_INPUT")) return false;

        display_ = wl_display_connect(nullptr);
        if (!display_) return false;
        registry_ = wl_display_get_registry(display_);
        if (!registry_) {
            stop();
            return false;
        }

        static const wl_registry_listener registry_listener = {
            &KwinInput::registry_global,
            &KwinInput::registry_remove,
        };
        wl_registry_add_listener(registry_, &registry_listener, this);
        if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0 || !fake_) {
            stop();
            return false;
        }
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(fake_)) <
            ORG_KDE_KWIN_FAKE_INPUT_KEYBOARD_KEY_SINCE_VERSION) {
            stop();
            return false;
        }

        org_kde_kwin_fake_input_authenticate(
            fake_, "OPAL", "Control the OPAL virtual remote desktop");
        (void)wl_display_flush(display_);
        return true;
    }

    bool apply(const opal::InputRecord& record, std::set<int>& held_keys,
               std::set<int>& held_buttons)
    {
        if (!fake_ || !display_) return false;
        switch (record.type) {
            case opal::InputRecordType::Key:
                if (record.a <= 0 || record.a > KEY_MAX || (record.b != 0 && record.b != 1)) return true;
                org_kde_kwin_fake_input_keyboard_key(
                    fake_, static_cast<std::uint32_t>(record.a),
                    record.b ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
                if (record.b) held_keys.insert(record.a); else held_keys.erase(record.a);
                break;
            case opal::InputRecordType::Pointer: {
                if (record.a < 0 || record.a > pointer_max || record.b < 0 || record.b > pointer_max) return true;
                const Bounds bounds = desktop_bounds();
                const double width = std::max(1, bounds.right - bounds.left);
                const double height = std::max(1, bounds.bottom - bounds.top);
                const double x = static_cast<double>(bounds.left) +
                    (static_cast<double>(record.a) / pointer_max) * std::max(0.0, width - 1.0);
                const double y = static_cast<double>(bounds.top) +
                    (static_cast<double>(record.b) / pointer_max) * std::max(0.0, height - 1.0);
                org_kde_kwin_fake_input_pointer_motion_absolute(
                    fake_, wl_fixed_from_double(x), wl_fixed_from_double(y));
                break;
            }
            case opal::InputRecordType::Button: {
                const int code = button_code(record.a);
                if (!code || (record.b != 0 && record.b != 1)) return true;
                org_kde_kwin_fake_input_button(
                    fake_, static_cast<std::uint32_t>(code),
                    record.b ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                if (record.b) held_buttons.insert(code); else held_buttons.erase(code);
                break;
            }
            case opal::InputRecordType::Wheel:
                if (record.a != 0)
                    org_kde_kwin_fake_input_axis(
                        fake_, WL_POINTER_AXIS_VERTICAL_SCROLL,
                        wl_fixed_from_double(static_cast<double>(-record.a) * 15.0));
                break;
            case opal::InputRecordType::Relative:
                if (record.a != 0 || record.b != 0)
                    org_kde_kwin_fake_input_pointer_motion(
                        fake_, wl_fixed_from_double(record.a), wl_fixed_from_double(record.b));
                break;
        }
        return flush();
    }

    void release_held(const std::set<int>& keys, const std::set<int>& buttons)
    {
        if (!fake_) return;
        for (int code : keys)
            org_kde_kwin_fake_input_keyboard_key(
                fake_, static_cast<std::uint32_t>(code), WL_KEYBOARD_KEY_STATE_RELEASED);
        for (int code : buttons)
            org_kde_kwin_fake_input_button(
                fake_, static_cast<std::uint32_t>(code), WL_POINTER_BUTTON_STATE_RELEASED);
        (void)flush();
    }

    bool active() const { return fake_ && display_; }

private:
    struct Bounds {
        int left = 0;
        int top = 0;
        int right = 1920;
        int bottom = 1080;
    };

    static void registry_global(void* data, wl_registry* registry, std::uint32_t name,
                                const char* interface, std::uint32_t version)
    {
        auto* self = static_cast<KwinInput*>(data);
        if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
            const std::uint32_t bind_version = std::min<std::uint32_t>(version, 6);
            self->fake_ = static_cast<org_kde_kwin_fake_input*>(
                wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, bind_version));
            return;
        }
        if (std::strcmp(interface, wl_output_interface.name) == 0) {
            auto output = std::make_unique<OutputState>();
            const std::uint32_t bind_version = std::min<std::uint32_t>(version, 4);
            output->output = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, bind_version));
            if (!output->output) return;
            static const wl_output_listener output_listener = {
                &KwinInput::output_geometry,
                &KwinInput::output_mode,
                &KwinInput::output_done,
                &KwinInput::output_scale,
                &KwinInput::output_name,
                &KwinInput::output_description,
            };
            wl_output_add_listener(output->output, &output_listener, output.get());
            self->outputs_.push_back(std::move(output));
        }
    }

    static void registry_remove(void*, wl_registry*, std::uint32_t) {}

    static void output_geometry(void* data, wl_output*, std::int32_t x, std::int32_t y,
                                std::int32_t, std::int32_t, std::int32_t,
                                const char*, const char*, std::int32_t)
    {
        auto* output = static_cast<OutputState*>(data);
        output->x = x;
        output->y = y;
    }

    static void output_mode(void* data, wl_output*, std::uint32_t flags,
                            std::int32_t width, std::int32_t height, std::int32_t)
    {
        auto* output = static_cast<OutputState*>(data);
        if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) return;
        output->width = width;
        output->height = height;
        output->current = width > 0 && height > 0;
    }

    static void output_done(void*, wl_output*) {}

    static void output_scale(void* data, wl_output*, std::int32_t scale)
    {
        static_cast<OutputState*>(data)->scale = std::max(1, static_cast<int>(scale));
    }

    static void output_name(void*, wl_output*, const char*) {}
    static void output_description(void*, wl_output*, const char*) {}

    Bounds desktop_bounds() const
    {
        bool found = false;
        Bounds result{};
        for (const auto& item : outputs_) {
            if (!item || !item->current || item->width <= 0 || item->height <= 0) continue;
            const int width = std::max(1, item->width / std::max(1, item->scale));
            const int height = std::max(1, item->height / std::max(1, item->scale));
            if (!found) {
                result.left = item->x;
                result.top = item->y;
                result.right = item->x + width;
                result.bottom = item->y + height;
                found = true;
            } else {
                result.left = std::min(result.left, item->x);
                result.top = std::min(result.top, item->y);
                result.right = std::max(result.right, item->x + width);
                result.bottom = std::max(result.bottom, item->y + height);
            }
        }
        if (!found) {
            result.left = 0;
            result.top = 0;
            result.right = env_dimension("OPAL_KWIN_VIRTUAL_WIDTH", 1920);
            result.bottom = env_dimension("OPAL_KWIN_VIRTUAL_HEIGHT", 1080);
        }
        return result;
    }

    bool flush()
    {
        if (!display_) return false;
        const int result = wl_display_flush(display_);
        return result >= 0 || errno == EAGAIN;
    }

    void stop()
    {
        if (fake_) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(fake_)) >=
                ORG_KDE_KWIN_FAKE_INPUT_DESTROY_SINCE_VERSION)
                org_kde_kwin_fake_input_destroy(fake_);
            else
                wl_proxy_destroy(reinterpret_cast<wl_proxy*>(fake_));
            fake_ = nullptr;
        }
        for (auto& output : outputs_) {
            if (output && output->output) wl_output_destroy(output->output);
        }
        outputs_.clear();
        if (registry_) wl_registry_destroy(registry_);
        registry_ = nullptr;
        if (display_) wl_display_disconnect(display_);
        display_ = nullptr;
    }

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    org_kde_kwin_fake_input* fake_ = nullptr;
    std::vector<std::unique_ptr<OutputState>> outputs_;
};

input_event make_event(int type, int code, int value)
{
    input_event event{};
    event.type = static_cast<__u16>(type);
    event.code = static_cast<__u16>(code);
    event.value = value;
    return event;
}

bool write_events(int fd, std::span<const input_event> events)
{
    if (fd < 0 || events.empty()) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(events.data());
    std::size_t remaining = events.size_bytes();
    while (remaining) {
        const ssize_t n = write(fd, bytes, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

int open_uinput() { return open("/dev/uinput", O_WRONLY | O_CLOEXEC); }

bool create_device(int fd, const char* name, unsigned short product)
{
    uinput_setup setup{};
    std::strncpy(setup.name, name, UINPUT_MAX_NAME_SIZE - 1);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x4f50;
    setup.id.product = product;
    return ioctl(fd, UI_DEV_SETUP, &setup) >= 0 && ioctl(fd, UI_DEV_CREATE) >= 0;
}

int create_keyboard()
{
    const int fd = open_uinput();
    if (fd < 0) return -1;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) { close(fd); return -1; }
    for (int i = 1; i <= KEY_MAX; ++i) (void)ioctl(fd, UI_SET_KEYBIT, i);
    if (!create_device(fd, "OPAL Remote Keyboard", 0x414b)) { close(fd); return -1; }
    return fd;
}

bool setup_abs_axis(int fd, int code)
{
    uinput_abs_setup axis{};
    axis.code = static_cast<__u16>(code);
    axis.absinfo.minimum = 0;
    axis.absinfo.maximum = pointer_max;
    axis.absinfo.resolution = 1;
    return ioctl(fd, UI_ABS_SETUP, &axis) >= 0;
}

int create_pointer()
{
    const int fd = open_uinput();
    if (fd < 0) return -1;
    bool ok = true;
    ok = ok && ioctl(fd, UI_SET_EVBIT, EV_KEY) >= 0;
    ok = ok && ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) >= 0;
    ok = ok && ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) >= 0;
    ok = ok && ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) >= 0;
    ok = ok && ioctl(fd, UI_SET_EVBIT, EV_ABS) >= 0;
    ok = ok && ioctl(fd, UI_SET_ABSBIT, ABS_X) >= 0;
    ok = ok && ioctl(fd, UI_SET_ABSBIT, ABS_Y) >= 0;
    ok = ok && ioctl(fd, UI_SET_EVBIT, EV_REL) >= 0;
    ok = ok && ioctl(fd, UI_SET_RELBIT, REL_X) >= 0;
    ok = ok && ioctl(fd, UI_SET_RELBIT, REL_Y) >= 0;
    ok = ok && ioctl(fd, UI_SET_RELBIT, REL_WHEEL) >= 0;
    ok = ok && ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER) >= 0;
    ok = ok && setup_abs_axis(fd, ABS_X) && setup_abs_axis(fd, ABS_Y);
    if (!ok || !create_device(fd, "OPAL Remote Pointer", 0x4150)) { close(fd); return -1; }
    return fd;
}

void destroy_device(int fd)
{
    if (fd < 0) return;
    (void)ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}

bool apply_uinput(int keyboard_fd, int pointer_fd, const opal::InputRecord& record,
                  std::set<int>& held_keys, std::set<int>& held_buttons)
{
    const auto sync = make_event(EV_SYN, SYN_REPORT, 0);
    switch (record.type) {
        case opal::InputRecordType::Key: {
            if (record.a <= 0 || record.a > KEY_MAX || (record.b != 0 && record.b != 1)) return true;
            const std::array events{make_event(EV_KEY, record.a, record.b), sync};
            if (!write_events(keyboard_fd, events)) return false;
            if (record.b) held_keys.insert(record.a); else held_keys.erase(record.a);
            return true;
        }
        case opal::InputRecordType::Pointer: {
            if (record.a < 0 || record.a > pointer_max || record.b < 0 || record.b > pointer_max) return true;
            const std::array events{make_event(EV_ABS, ABS_X, record.a), make_event(EV_ABS, ABS_Y, record.b), sync};
            return write_events(pointer_fd, events);
        }
        case opal::InputRecordType::Button: {
            const int code = button_code(record.a);
            if (!code || (record.b != 0 && record.b != 1)) return true;
            const std::array events{make_event(EV_KEY, code, record.b), sync};
            if (!write_events(pointer_fd, events)) return false;
            if (record.b) held_buttons.insert(code); else held_buttons.erase(code);
            return true;
        }
        case opal::InputRecordType::Wheel: {
            const std::array events{make_event(EV_REL, REL_WHEEL, record.a), sync};
            return write_events(pointer_fd, events);
        }
        case opal::InputRecordType::Relative: {
            if (record.a == 0 && record.b == 0) return true;
            const std::array events{make_event(EV_REL, REL_X, record.a), make_event(EV_REL, REL_Y, record.b), sync};
            return write_events(pointer_fd, events);
        }
    }
    return true;
}

bool apply_record(KwinInput* kwin, int keyboard_fd, int pointer_fd,
                  const opal::InputRecord& record, std::set<int>& held_keys,
                  std::set<int>& held_buttons)
{
    if (kwin && kwin->active()) return kwin->apply(record, held_keys, held_buttons);
    return apply_uinput(keyboard_fd, pointer_fd, record, held_keys, held_buttons);
}

bool consume_pending(std::vector<std::uint8_t>& pending, KwinInput* kwin,
                     int keyboard_fd, int pointer_fd, std::set<int>& held_keys,
                     std::set<int>& held_buttons, bool eof)
{
    for (;;) {
        if (pending.empty()) return true;
        const bool binary = pending.size() >= 4 && pending[0] == 'O' && pending[1] == 'P' &&
                            pending[2] == 'I' && pending[3] == 'N';
        if (binary) {
            if (pending.size() < opal::kInputRecordBytes) return !eof;
            opal::InputRecord record;
            if (!opal::decode_input_record(
                    std::span<const std::uint8_t>(pending.data(), opal::kInputRecordBytes), record))
                return false;
            if (!apply_record(kwin, keyboard_fd, pointer_fd, record, held_keys, held_buttons)) return false;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(opal::kInputRecordBytes));
            continue;
        }

        const auto newline = std::find(pending.begin(), pending.end(), static_cast<std::uint8_t>('\n'));
        if (newline == pending.end()) {
            if (!eof) {
                if (pending.size() > 512) return false;
                return true;
            }
            if (pending.empty()) return true;
        }
        const std::size_t length = newline == pending.end() ? pending.size() :
                                   static_cast<std::size_t>(newline - pending.begin());
        const std::string_view line(reinterpret_cast<const char*>(pending.data()), length);
        opal::InputRecord record;
        if (!line.empty() && opal::parse_input_command(line, record) &&
            !apply_record(kwin, keyboard_fd, pointer_fd, record, held_keys, held_buttons))
            return false;
        const std::size_t consumed = length + (newline == pending.end() ? 0 : 1);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (newline == pending.end()) return true;
    }
}

void release_uinput_held(int keyboard_fd, int pointer_fd,
                         const std::set<int>& held_keys, const std::set<int>& held_buttons)
{
    for (int code : held_keys) {
        const std::array events{make_event(EV_KEY, code, 0), make_event(EV_SYN, SYN_REPORT, 0)};
        (void)write_events(keyboard_fd, events);
    }
    for (int code : held_buttons) {
        const std::array events{make_event(EV_KEY, code, 0), make_event(EV_SYN, SYN_REPORT, 0)};
        (void)write_events(pointer_fd, events);
    }
}

}

int main()
{
    KwinInput kwin;
    const bool kwin_active = kwin.start();

    int keyboard_fd = -1;
    int pointer_fd = -1;
    if (!kwin_active) {
        keyboard_fd = create_keyboard();
        if (keyboard_fd < 0) { perror("OPAL keyboard uinput"); return 1; }
        pointer_fd = create_pointer();
        if (pointer_fd < 0) {
            perror("OPAL pointer uinput");
            destroy_device(keyboard_fd);
            return 1;
        }
    }

    std::set<int> held_keys;
    std::set<int> held_buttons;
    std::vector<std::uint8_t> pending;
    pending.reserve(1024);
    std::array<std::uint8_t, 1024> chunk{};
    bool ok = true;

    for (;;) {
        const ssize_t n = read(STDIN_FILENO, chunk.data(), chunk.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) {
            ok = consume_pending(pending, kwin_active ? &kwin : nullptr,
                                 keyboard_fd, pointer_fd, held_keys, held_buttons, true);
            break;
        }
        pending.insert(pending.end(), chunk.begin(), chunk.begin() + n);
        if (!consume_pending(pending, kwin_active ? &kwin : nullptr,
                             keyboard_fd, pointer_fd, held_keys, held_buttons, false)) {
            ok = false;
            break;
        }
    }

    if (kwin_active) kwin.release_held(held_keys, held_buttons);
    else release_uinput_held(keyboard_fd, pointer_fd, held_keys, held_buttons);
    destroy_device(pointer_fd);
    destroy_device(keyboard_fd);
    return ok ? 0 : 1;
}
#else
#include <iostream>
int main() { std::cerr << "opal-input is Linux-only\n"; return 1; }
#endif
