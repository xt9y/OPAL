#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <openssl/ssl.h>

namespace opal {
struct VideoKeys {
    std::array<std::uint8_t,32> send_key{},recv_key{};
    std::array<std::uint8_t,12> send_nonce_base{},recv_nonce_base{};
};

bool derive_video_keys(SSL*,std::string_view session_token,
    std::string_view client_pub,std::string_view host_fp,bool client_side,VideoKeys&);
bool seal_video_datagram(const VideoKeys&,std::uint64_t seq,
    std::span<const std::uint8_t> aad,std::span<const std::uint8_t> plaintext,
    std::vector<std::uint8_t>& ciphertext_tag);
bool open_video_datagram(const VideoKeys&,std::uint64_t seq,
    std::span<const std::uint8_t> aad,std::span<const std::uint8_t> ciphertext_tag,
    std::vector<std::uint8_t>& plaintext);

class ReplayWindow1024 {
public:
    bool accept(std::uint64_t);
    void reset();
private:
    bool initialized_=false;
    std::uint64_t highest_=0;
    std::bitset<1024> seen_{};
};
}
