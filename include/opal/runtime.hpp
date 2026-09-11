#pragma once

#include <filesystem>

namespace opal {

class ClientRuntimeLease {
public:
    explicit ClientRuntimeLease(std::filesystem::path root);
    ClientRuntimeLease(const ClientRuntimeLease&) = delete;
    ClientRuntimeLease& operator=(const ClientRuntimeLease&) = delete;
    ~ClientRuntimeLease();

    bool acquire();
    bool active() const noexcept { return active_; }

private:
    std::filesystem::path root_;
    unsigned long long pid_ = 0;
    bool active_ = false;
};

bool client_runtime_running(const std::filesystem::path& root);
bool stop_client_runtime(const std::filesystem::path& root);

}
