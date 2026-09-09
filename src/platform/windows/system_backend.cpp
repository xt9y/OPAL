#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/platform.hpp>
#include <opal/platform_error.hpp>
#include <opal/system.hpp>
#include <opal/tailnet.hpp>

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <combaseapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace opal {
namespace {

constexpr wchar_t kHostRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kHostRunValueName[] = L"OPAL Host";

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool owns = SUCCEEDED(result);
    ~ComScope() { if (owns) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

template <class T>
void release(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};

std::filesystem::path current_executable_path()
{
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1)
            return std::filesystem::path(std::wstring(buffer.data(), length));
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path input_helper_path()
{
    if (const char* configured = std::getenv("OPAL_INPUT_HELPER"); configured && *configured) {
        std::error_code error;
        if (std::filesystem::is_regular_file(configured, error) && !error) return configured;
    }
    const auto executable = current_executable_path();
    if (executable.empty()) return {};
    std::error_code error;
    const auto adjacent = executable.parent_path() / "opal-input.exe";
    if (std::filesystem::is_regular_file(adjacent, error) && !error) return adjacent;
    const auto dev = executable.parent_path() / "build" / "opal-input.exe";
    error.clear();
    if (std::filesystem::is_regular_file(dev, error) && !error) return dev;
    return {};
}

bool sdl_video_available(std::string& driver)
{
    const bool initialized = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
    if (!initialized && !SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    const char* name = SDL_GetCurrentVideoDriver();
    driver = name && *name ? name : "unknown";
    if (!initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return !driver.empty();
}

bool media_foundation_h264_hardware_encoder_available()
{
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return false;
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, &output, &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) if (activates[i]) activates[i]->Release();
        CoTaskMemFree(activates);
    }
    MFShutdown();
    return SUCCEEDED(hr) && count > 0;
}

bool dxgi_desktop_duplication_available()
{
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;

    bool ok = SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory &&
              SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter &&
              SUCCEEDED(adapter->EnumOutputs(0, &output)) && output &&
              SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1))) && output1;
    if (ok) {
        ok = SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) && device;
    }
    if (ok) ok = SUCCEEDED(output1->DuplicateOutput(device, &duplication)) && duplication;

    release(duplication);
    release(context);
    release(device);
    release(output1);
    release(output);
    release(adapter);
    release(factory);
    return ok;
}

bool wasapi_render_endpoint_available()
{
    ComScope com;
    if (!com.usable()) return false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    const bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
                    enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) && endpoint;
    release(endpoint);
    release(enumerator);
    return ok;
}

std::wstring host_autostart_command()
{
    const auto executable = current_executable_path();
    if (executable.empty()) return {};
    return L"\"" + executable.native() + L"\" --internal-host-daemon";
}

bool register_host_autostart()
{
    const auto command = host_autostart_command();
    if (command.empty()) return false;

    RegistryKey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kHostRunKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key.value, nullptr) != ERROR_SUCCESS)
        return false;

    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key.value, kHostRunValueName, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(command.c_str()), bytes) == ERROR_SUCCESS;
}

bool host_autostart_registered()
{
    const auto expected = host_autostart_command();
    if (expected.empty()) return false;

    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kHostRunKeyPath, 0, KEY_QUERY_VALUE, &key.value) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.value, kHostRunValueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        return false;

    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key.value, kHostRunValueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
        return false;

    return CompareStringOrdinal(value.data(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool remove_host_autostart()
{
    RegistryKey key;
    const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, kHostRunKeyPath, 0, KEY_SET_VALUE, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) return false;

    const LSTATUS removed = RegDeleteValueW(key.value, kHostRunValueName);
    return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
}

std::filesystem::path host_pid_path()
{
    return Paths::load().root / "host.pid";
}

void clear_host_pid()
{
    std::error_code error;
    std::filesystem::remove(host_pid_path(), error);
}

bool read_host_pid(DWORD& pid)
{
    std::ifstream input(host_pid_path());
    unsigned long long value = 0;
    if (!(input >> value) || value == 0 || value > 0xffffffffULL) return false;
    pid = static_cast<DWORD>(value);
    return true;
}

bool process_is_opal(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length) || length == 0) return false;
    const std::filesystem::path image(std::wstring(buffer.data(), length));
    return CompareStringOrdinal(image.filename().c_str(), -1, L"opal.exe", -1, TRUE) == CSTR_EQUAL;
}

bool host_daemon_running()
{
    DWORD pid = 0;
    if (!read_host_pid(pid)) return false;

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) clear_host_pid();
        return false;
    }

    DWORD exit_code = 0;
    const bool running = process_is_opal(process) &&
                         GetExitCodeProcess(process, &exit_code) &&
                         exit_code == STILL_ACTIVE;
    CloseHandle(process);
    if (!running) clear_host_pid();
    return running;
}

bool stop_host_daemon()
{
    DWORD pid = 0;
    if (!read_host_pid(pid)) {
        clear_host_pid();
        return true;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            clear_host_pid();
            return true;
        }
        return false;
    }

    if (!process_is_opal(process)) {
        CloseHandle(process);
        clear_host_pid();
        return true;
    }

    const DWORD wait = WaitForSingleObject(process, 0);
    if (wait == WAIT_OBJECT_0) {
        CloseHandle(process);
        clear_host_pid();
        return true;
    }

    const bool terminated = TerminateProcess(process, 0) != FALSE;
    if (terminated) (void)WaitForSingleObject(process, 3000);
    CloseHandle(process);
    if (terminated) clear_host_pid();
    return terminated;
}

bool launch_host_daemon()
{
    if (host_daemon_running()) return true;

    const auto executable_path = current_executable_path();
    if (executable_path.empty()) return false;
    const std::wstring executable = executable_path.native();
    const std::wstring working = executable_path.parent_path().native();
    std::wstring command = L"\"" + executable + L"\" --internal-host-daemon";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
        working.empty() ? nullptr : working.c_str(), &startup, &process);
    if (!created) {
        if (const char* debug = std::getenv("OPAL_DEBUG"); debug && *debug && std::string(debug) != "0")
            std::cerr << "OPAL host daemon CreateProcessW failed error=" << GetLastError() << '\n';
        return false;
    }

    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 300);
    bool running = wait == WAIT_TIMEOUT;
    if (!running && wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process.hProcess, &exit_code)) {
            if (const char* debug = std::getenv("OPAL_DEBUG"); debug && *debug && std::string(debug) != "0")
                std::cerr << "OPAL host daemon exited during startup code=" << exit_code << '\n';
        }
    }
    CloseHandle(process.hProcess);
    return running;
}

void write_default_config(const Paths& paths)
{
    if (std::filesystem::exists(paths.config)) return;
    Ini config;
    config.set("video", "fps", "60");
    config.set("video", "bitrate_kbps", "30000");
    config.set("video", "fullscreen", "true");
    config.set("audio", "enabled", "true");
    config.set("network", "mode", "opal-native");
    config.set("network", "transport", "rendezvous+direct-udp+relay");
    (void)config.save(paths.config);
}

void show_doctor_item(const std::string& name, bool ok)
{
    std::cout << (ok ? "[ok]   " : "[warn] ") << name << '\n';
}

void show_doctor_failure(const std::string& name, PlatformComponent component, PlatformFailure failure)
{
    std::cout << "[warn] " << name << " component=" << platform_component_name(component)
              << " failure=" << platform_failure_name(failure) << '\n';
}

}

int ensure_tailnet()
{
    if (!tailscale_cli_available()) {
        std::cerr << "Tailscale is not installed; continuing with LAN/rendezvous/relay connectivity.\n";
        return 1;
    }
    if (!tailscale_connected()) {
        std::cerr << "Tailscale is installed but not connected. Connect Tailscale, then retry.\n";
        return 1;
    }
    return 0;
}

int init()
{
    const auto paths = Paths::load();
    if (!ensure_layout(paths) || !ensure_identity(paths.identity_key, paths.identity_pub)) return 1;
    write_default_config(paths);
    std::cout << "Initialized " << paths.root << '\n';
    return 0;
}

int doctor()
{
    const auto paths = Paths::load();
    std::cout << "OPAL doctor\n";
    std::cout << "[info] platform=" << platform_name(current_platform()) << '\n';

    std::string driver;
    const bool sdl_ok = sdl_video_available(driver);
    if (sdl_ok) show_doctor_item("SDL3 client video backend (" + driver + ")", true);
    else show_doctor_failure("SDL3 client video backend unavailable", PlatformComponent::Presenter, PlatformFailure::Unavailable);

    if (avcodec_find_decoder(AV_CODEC_ID_H264)) show_doctor_item("Linked FFmpeg H.264 decoder", true);
    else show_doctor_failure("Linked FFmpeg H.264 decoder", PlatformComponent::Decoder, PlatformFailure::DependencyMissing);

    if (media_foundation_h264_hardware_encoder_available()) show_doctor_item("Media Foundation H.264 hardware encoder", true);
    else show_doctor_failure("Media Foundation H.264 hardware encoder", PlatformComponent::Encoder, PlatformFailure::Unavailable);

    if (dxgi_desktop_duplication_available()) show_doctor_item("DXGI Desktop Duplication", true);
    else show_doctor_failure("DXGI Desktop Duplication", PlatformComponent::Capture, PlatformFailure::Unavailable);

    if (wasapi_render_endpoint_available()) show_doctor_item("WASAPI loopback render endpoint", true);
    else show_doctor_failure("WASAPI loopback render endpoint", PlatformComponent::AudioCapture, PlatformFailure::Unavailable);

    show_doctor_item("SendInput helper present", !input_helper_path().empty());
    if (tailscale_connected()) show_doctor_item("Tailscale WAN underlay connected", true);
    else if (tailscale_cli_available()) show_doctor_failure("Tailscale WAN underlay installed but disconnected", PlatformComponent::Datagram, PlatformFailure::Unavailable);
    else show_doctor_failure("Tailscale WAN underlay", PlatformComponent::Datagram, PlatformFailure::DependencyMissing);

    show_doctor_item("Host auto-start registered", host_autostart_registered());
    show_doctor_item("Host daemon running", host_daemon_running());
    show_doctor_item("OPAL state initialized", std::filesystem::exists(paths.root));
    std::cout << "[info] client presenter=sdl3 decoder=libavcodec clipboard=win32-unicode\n";
    std::cout << "[info] host capture=dxgi-desktop-duplication encoder=media-foundation-hardware-lowlatency input=sendinput clipboard=win32-unicode audio=wasapi-loopback+aac\n";
    return 0;
}

int host_service(bool enable)
{
    if (!enable) {
        const bool autostart_removed = remove_host_autostart();
        const bool daemon_stopped = stop_host_daemon();
        if (!autostart_removed)
            std::cerr << "Could not remove OPAL Windows auto-start entry.\n";
        if (!daemon_stopped)
            std::cerr << "Could not stop OPAL host daemon.\n";
        return autostart_removed && daemon_stopped ? 0 : 1;
    }

    if (current_executable_path().empty()) {
        std::cerr << "Could not resolve OPAL executable path.\n";
        return 1;
    }
    if (!register_host_autostart()) {
        std::cerr << "Could not register OPAL Windows auto-start entry.\n";
        return 1;
    }
    if (!launch_host_daemon()) {
        std::cerr << "Could not start OPAL host daemon.\n";
        return 1;
    }
    return 0;
}

int restart_services()
{
    if (!host_autostart_registered()) {
        std::cerr << "OPAL host auto-start is not installed. Run host setup first.\n";
        return 1;
    }
    if (!stop_host_daemon()) {
        std::cerr << "Could not stop OPAL host daemon.\n";
        return 1;
    }
    if (!launch_host_daemon()) {
        std::cerr << "Could not restart OPAL host daemon.\n";
        return 1;
    }
    std::cout << "OPAL host daemon restarted.\n";
    return 0;
}

int clean()
{
    (void)host_service(false);
    const auto paths = Paths::load();
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
    if (error) {
        std::cerr << "Could not remove OPAL state: " << error.message() << '\n';
        return 1;
    }
    std::cout << "OPAL state cleaned.\n";
    return 0;
}

int bridge_setup(const char* mac)
{
    if (!mac || !*mac) {
        std::cerr << "--mac required\n";
        return 2;
    }
    const auto paths = Paths::load();
    if (!ensure_layout(paths)) return 1;
    Ini config;
    config.set("bridge", "mac", mac);
    config.set("bridge", "secret", random_hex(32));
    if (!config.save(paths.root / "bridge.ini")) return 1;
    std::cout << "Bridge configured for " << mac << '\n';
    return 0;
}

}
