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
#include <tlhelp32.h>

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
constexpr wchar_t kHostMutexName[] = L"Local\\xt9y.OPAL.HostDaemon";
constexpr wchar_t kHostStopEventName[] = L"Local\\xt9y.OPAL.HostStop";

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

bool debug_enabled()
{
    const char* value = std::getenv("OPAL_DEBUG");
    return value && *value && std::string(value) != "0";
}

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

void clear_host_pid_if(DWORD expected_pid)
{
    std::ifstream input(host_pid_path());
    unsigned long long pid = 0;
    if (!(input >> pid) || pid != expected_pid) return;
    input.close();
    clear_host_pid();
}

bool write_host_pid(DWORD pid)
{
    const auto paths = Paths::load();
    if (!ensure_layout(paths)) return false;
    std::ofstream output(paths.root / "host.pid", std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << pid << '\n';
    return output.good();
}

bool read_host_pid(DWORD& pid)
{
    std::ifstream input(host_pid_path());
    unsigned long long value = 0;
    if (!(input >> value) || value == 0 || value > 0xffffffffULL) return false;
    pid = static_cast<DWORD>(value);
    return true;
}

bool process_is_current_opal(HANDLE process)
{
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length) || length == 0) return false;
    const std::filesystem::path image(std::wstring(buffer.data(), length));
    const auto current = current_executable_path();
    if (current.empty()) return false;
    return CompareStringOrdinal(image.native().c_str(), -1, current.native().c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool host_mutex_running()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName);
    if (!mutex) return false;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    const bool running = wait == WAIT_TIMEOUT;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) (void)ReleaseMutex(mutex);
    CloseHandle(mutex);
    return running;
}

bool wait_for_host_mutex_release(DWORD timeout_ms)
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName);
    if (!mutex) return true;
    const DWORD wait = WaitForSingleObject(mutex, timeout_ms);
    const bool released = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (released) (void)ReleaseMutex(mutex);
    CloseHandle(mutex);
    return released;
}

bool running_process(DWORD pid, DWORD access, HANDLE& process)
{
    process = OpenProcess(access | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exit_code = 0;
    if (!process_is_current_opal(process) ||
        !GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) {
        CloseHandle(process);
        process = nullptr;
        return false;
    }
    return true;
}

bool recover_host_pid(DWORD& pid)
{
    DWORD stored = 0;
    HANDLE process = nullptr;
    if (read_host_pid(stored) && running_process(stored, 0, process)) {
        CloseHandle(process);
        pid = stored;
        return true;
    }
    if (process) CloseHandle(process);
    clear_host_pid();
    if (!host_mutex_running()) return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD candidate = 0;
    unsigned matches = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            if (CompareStringOrdinal(entry.szExeFile, -1, L"opal.exe", -1, TRUE) != CSTR_EQUAL) continue;
            HANDLE candidate_process = nullptr;
            if (!running_process(entry.th32ProcessID, 0, candidate_process)) continue;
            CloseHandle(candidate_process);
            candidate = entry.th32ProcessID;
            ++matches;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (matches != 1 || candidate == 0) return false;
    (void)write_host_pid(candidate);
    pid = candidate;
    return true;
}

bool host_daemon_running()
{
    return host_mutex_running();
}

bool stop_host_daemon()
{
    if (!host_mutex_running()) {
        clear_host_pid();
        return true;
    }

    HANDLE stop_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kHostStopEventName);
    if (stop_event) {
        const bool signaled = SetEvent(stop_event) != FALSE;
        CloseHandle(stop_event);
        if (signaled && wait_for_host_mutex_release(3000)) {
            clear_host_pid();
            return true;
        }
    }

    if (!host_mutex_running()) {
        clear_host_pid();
        return true;
    }

    DWORD pid = 0;
    if (!recover_host_pid(pid)) {
        if (debug_enabled())
            std::cerr << "OPAL host daemon is alive but its process could not be identified safely.\n";
        return false;
    }

    HANDLE process = nullptr;
    if (!running_process(pid, PROCESS_TERMINATE, process)) {
        clear_host_pid_if(pid);
        return !host_mutex_running();
    }

    const bool terminated = TerminateProcess(process, 0) != FALSE;
    if (terminated) (void)WaitForSingleObject(process, 3000);
    CloseHandle(process);
    if (terminated) clear_host_pid_if(pid);
    return terminated && wait_for_host_mutex_release(3000);
}

bool launch_host_daemon()
{
    if (host_mutex_running()) return true;

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
        if (debug_enabled())
            std::cerr << "OPAL host daemon CreateProcessW failed error=" << GetLastError() << '\n';
        return false;
    }

    CloseHandle(process.hThread);
    const ULONGLONG deadline = GetTickCount64() + 1500;
    for (;;) {
        if (host_mutex_running()) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
                (void)write_host_pid(process.dwProcessId);
            CloseHandle(process.hProcess);
            return true;
        }

        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            (void)GetExitCodeProcess(process.hProcess, &exit_code);
            CloseHandle(process.hProcess);
            clear_host_pid_if(process.dwProcessId);
            if (host_mutex_running()) return true;
            if (debug_enabled())
                std::cerr << "OPAL host daemon exited during startup code=" << exit_code << '\n';
            return false;
        }

        if (GetTickCount64() >= deadline) break;
        Sleep(10);
    }

    if (debug_enabled()) std::cerr << "OPAL host daemon did not publish its ready mutex in time.\n";
    (void)TerminateProcess(process.hProcess, 1);
    (void)WaitForSingleObject(process.hProcess, 3000);
    CloseHandle(process.hProcess);
    clear_host_pid_if(process.dwProcessId);
    return false;
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
    if (!register_host_autostart()) {
        std::cerr << "Could not register OPAL Windows auto-start entry.\n";
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
    if (host_service(false) != 0) {
        std::cerr << "Could not clean OPAL while the host daemon is still running.\n";
        return 1;
    }
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
