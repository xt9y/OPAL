#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace opal {

constexpr std::size_t kReliableControlMaxPending=64;
constexpr std::size_t kReliableControlMaxPayload=768;

struct ReliableTransmission {
    std::uint64_t sequence=0;
    std::string payload;
    std::uint32_t attempt=0;
};

struct ReliableAckState {
    std::uint64_t sequence=0;
    std::uint32_t bits=0;
};

class ReliableControlSender {
public:
    std::uint64_t enqueue(std::string payload,std::uint64_t now_ms);
    std::vector<ReliableTransmission> due(std::uint64_t now_ms);
    void acknowledge(ReliableAckState);
    std::size_t pending() const;
    bool failed() const;
    void reset();
private:
    struct Pending {std::string payload;std::uint64_t next_send_ms=0;std::uint32_t attempt=0;};
    std::map<std::uint64_t,Pending> pending_;
    std::uint64_t next_sequence_=1;
    bool failed_=false;
};

class ReliableControlReceiver {
public:
    bool receive(std::uint64_t sequence,std::string payload,std::vector<std::string>&delivered);
    ReliableAckState ack_state() const;
    std::size_t buffered() const;
    void reset();
private:
    void note_seen(std::uint64_t sequence);
    std::map<std::uint64_t,std::string> buffered_;
    std::uint64_t next_expected_=1,highest_seen_=0;
    std::uint32_t seen_bits_=0;
};

class LatestPointerReceiver {
public:
    bool accept(std::uint64_t sequence,std::string payload);
    const std::string& latest() const;
    std::uint64_t sequence() const;
    void reset();
private:
    std::uint64_t sequence_=0;
    std::string latest_;
};

}
