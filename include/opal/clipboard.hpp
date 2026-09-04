#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace opal {

constexpr std::size_t kClipboardMaxBytes=16*1024;
constexpr std::size_t kClipboardChunkBytes=640;
constexpr std::size_t kClipboardControlMaxBytes=768;

enum class ClipboardReceiveStatus {
    NotClipboard,
    Accepted,
    Complete,
    Rejected
};

class ClipboardSender {
public:
    void prime_local(std::string_view text);
    bool observe_local(std::string_view text);
    void note_remote_applied(std::string_view text);
    void restart_transport();
    const std::string* next_message() const;
    void pop_message();
    std::size_t queued_messages() const;
private:
    void queue_current();
    std::string last_local_;
    std::string transfer_text_;
    std::deque<std::string> outbound_;
    bool transfer_active_=false;
    std::uint64_t next_transfer_id_=1;
    bool primed_=false;
};

class ClipboardReceiver {
public:
    ClipboardReceiveStatus receive(std::string_view message,std::string& completed);
    void reset();
private:
    std::uint64_t transfer_id_=0;
    std::size_t total_=0;
    std::string buffer_;
};

}
