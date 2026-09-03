#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <opal/media.hpp>

namespace opal {
struct SessionOptions {
    std::string target;
    int control_port=47990;
    int video_port=47991;
    bool tunneled=false;
    std::string control_token;
    std::string video_token;
    std::string fingerprint;
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
    std::string fingerprint() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
