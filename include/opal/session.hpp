#pragma once
#include <functional>
#include <memory>
#include <string>
#include <opal/media_profile.hpp>

namespace opal {
struct SessionOptions {
    std::string rendezvous_id;
    std::string expected_host_public_key;
    std::string tailnet_address;
    std::string client_public_key;
    std::string client_private_key_path;
    bool paired=false;
    std::string pairing_password;
    std::function<std::string()> pairing_password_provider;
    std::string label;
    StreamOptions stream;
};

class SessionSupervisor {
public:
    explicit SessionSupervisor(SessionOptions options);
    ~SessionSupervisor();
    SessionSupervisor(const SessionSupervisor&)=delete;
    SessionSupervisor& operator=(const SessionSupervisor&)=delete;

    bool start();
    void stop();
    bool send_input(const std::string &command);
    unsigned long control_generation() const;
    bool media_started() const;
    bool running() const;
    bool paired() const;
    int remote_width() const;
    int remote_height() const;
    std::string remote_mac() const;
    std::string remote_tailnet_address() const;
    std::string host_public_key() const;
    std::string fingerprint() const;
    std::string path_name() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
