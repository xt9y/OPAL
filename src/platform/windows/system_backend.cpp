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
#include <oleauto.h>
#include <taskschd.h>

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
constexpr wchar_t kHostTaskName[] = L"OPAL Host";

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

struct TaskConnection {
    ITaskService* service = nullptr;
    ITaskFolder* root = nullptr;
    ~TaskConnection() { release(root); release(service); }
};

bool connect_task_scheduler(TaskConnection& connection)
{
    ComScope com;
    if (!com.usable()) return false;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&connection.service));
    if (FAILED(hr) || !connection.service) return false;
    VARIANT empty;
    VariantInit(&empty);
    hr = connection.service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return false;
    BSTR root_path = SysAllocString(L"\\");
    if (!root_path) return false;
    hr = connection.service->GetFolder(root_path, &connection.root);
    SysFreeString(root_path);
    return SUCCEEDED(hr) && connection.root;
}

bool task_exists()
{
    ComScope com;
    if (!com.usable()) return false;
    TaskConnection connection;
    if (!connect_task_scheduler(connection)) return false;
    BSTR name = SysAllocString(kHostTaskName);
    if (!name) return false;
    IRegisteredTask* task = nullptr;
    const HRESULT hr = connection.root->GetTask(name, &task);
    SysFreeString(name);
    release(task);
    return SUCCEEDED(hr);
}

bool register_host_task()
{
    ComScope com;
    if (!com.usable()) return false;
    TaskConnection connection;
    if (!connect_task_scheduler(connection)) return false;

    const auto executable_path = current_executable_path();
    if (executable_path.empty()) return false;
    const std::wstring executable = executable_path.native();
    const std::wstring working = executable_path.parent_path().native();
    if (executable.empty()) return false;

    ITaskDefinition* definition = nullptr;
    if (FAILED(connection.service->NewTask(0, &definition)) || !definition) return false;

    bool ok = true;
    IRegistrationInfo* registration = nullptr;
    if (SUCCEEDED(definition->get_RegistrationInfo(&registration)) && registration) {
        BSTR description = SysAllocString(L"OPAL interactive remote desktop host");
        ok = description && SUCCEEDED(registration->put_Description(description));
        SysFreeString(description);
    } else ok = false;
    release(registration);

    const bool elevated = [] {
        const char* value = std::getenv("OPAL_WINDOWS_HOST_ELEVATED");
        return value && *value && std::string(value) != "0";
    }();
    if (ok && elevated) {
        IPrincipal* principal = nullptr;
        if (SUCCEEDED(definition->get_Principal(&principal)) && principal)
            ok = SUCCEEDED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
        else
            ok = false;
        release(principal);
    }

    ITriggerCollection* triggers = nullptr;
    ITrigger* trigger = nullptr;
    if (ok && SUCCEEDED(definition->get_Triggers(&triggers)) && triggers &&
        SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger) {
        (void)trigger->put_Enabled(VARIANT_TRUE);
    } else ok = false;
    release(trigger);
    release(triggers);

    IActionCollection* actions = nullptr;
    IAction* action = nullptr;
    IExecAction* exec = nullptr;
    if (ok && SUCCEEDED(definition->get_Actions(&actions)) && actions &&
        SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action &&
        SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec))) && exec) {
        BSTR path = SysAllocString(executable.c_str());
        BSTR arguments = SysAllocString(L"--internal-host-daemon");
        BSTR directory = working.empty() ? nullptr : SysAllocString(working.c_str());
        ok = path && arguments && SUCCEEDED(exec->put_Path(path)) && SUCCEEDED(exec->put_Arguments(arguments));
        if (ok && directory) ok = SUCCEEDED(exec->put_WorkingDirectory(directory));
        SysFreeString(directory);
        SysFreeString(arguments);
        SysFreeString(path);
    } else ok = false;
    release(exec);
    release(action);
    release(actions);

    ITaskSettings* settings = nullptr;
    if (ok && SUCCEEDED(definition->get_Settings(&settings)) && settings) {
        (void)settings->put_StartWhenAvailable(VARIANT_TRUE);
        (void)settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        (void)settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        (void)settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        BSTR unlimited = SysAllocString(L"PT0S");
        if (unlimited) {
            (void)settings->put_ExecutionTimeLimit(unlimited);
            SysFreeString(unlimited);
        }
    } else ok = false;
    release(settings);

    IRegisteredTask* registered = nullptr;
    if (ok) {
        VARIANT empty;
        VariantInit(&empty);
        BSTR name = SysAllocString(kHostTaskName);
        if (!name) ok = false;
        else {
            const HRESULT hr = connection.root->RegisterTaskDefinition(
                name, definition, TASK_CREATE_OR_UPDATE, empty, empty,
                TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
            if (FAILED(hr)) {
                if (const char* debug = std::getenv("OPAL_DEBUG"); debug && *debug && std::string(debug) != "0")
                    std::cerr << "OPAL Task Scheduler registration failed HRESULT="
                              << static_cast<unsigned long>(hr) << '\n';
            }
            ok = SUCCEEDED(hr) && registered;
            SysFreeString(name);
        }
    }
    release(registered);
    release(definition);
    return ok;
}

bool delete_host_task()
{
    ComScope com;
    if (!com.usable()) return false;
    TaskConnection connection;
    if (!connect_task_scheduler(connection)) return false;
    BSTR name = SysAllocString(kHostTaskName);
    if (!name) return false;
    const HRESULT hr = connection.root->DeleteTask(name, 0);
    SysFreeString(name);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool run_host_task(bool restart)
{
    ComScope com;
    if (!com.usable()) return false;
    TaskConnection connection;
    if (!connect_task_scheduler(connection)) return false;
    BSTR name = SysAllocString(kHostTaskName);
    if (!name) return false;
    IRegisteredTask* task = nullptr;
    HRESULT hr = connection.root->GetTask(name, &task);
    SysFreeString(name);
    if (FAILED(hr) || !task) return false;
    if (restart) (void)task->Stop(0);
    VARIANT empty;
    VariantInit(&empty);
    IRunningTask* running = nullptr;
    hr = task->Run(empty, &running);
    release(running);
    release(task);
    return SUCCEEDED(hr);
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

    show_doctor_item("Interactive host logon task installed", task_exists());
    show_doctor_item("OPAL state initialized", std::filesystem::exists(paths.root));
    std::cout << "[info] client presenter=sdl3 decoder=libavcodec clipboard=win32-unicode\n";
    std::cout << "[info] host capture=dxgi-desktop-duplication encoder=media-foundation-hardware-lowlatency input=sendinput clipboard=win32-unicode audio=wasapi-loopback+aac\n";
    return 0;
}

int host_service(bool enable)
{
    if (!enable) return delete_host_task() ? 0 : 1;
    if (current_executable_path().empty()) {
        std::cerr << "Could not resolve OPAL executable path.\n";
        return 1;
    }
    if (!register_host_task()) {
        std::cerr << "Could not register OPAL interactive logon task.\n";
        return 1;
    }
    if (!run_host_task(false)) {
        std::cerr << "OPAL host task was registered but could not be started.\n";
        return 1;
    }
    return 0;
}

int restart_services()
{
    if (!task_exists()) {
        std::cerr << "OPAL host task is not installed. Run host setup first.\n";
        return 1;
    }
    if (!run_host_task(true)) {
        std::cerr << "Could not restart OPAL host task.\n";
        return 1;
    }
    std::cout << "OPAL host task restarted.\n";
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
