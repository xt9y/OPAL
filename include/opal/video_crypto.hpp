#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace opal {
struct VideoKeys {
    std::array<std::uint8_t,32> send_key{},recv_key{};
    std::array<std::uint8_t,12> send_nonce_base{},recv_nonce_base{};
};

class VideoCipher {
public:
    explicit VideoCipher(const VideoKeys&);
    ~VideoCipher();
    VideoCipher(const VideoCipher&)=delete;
    VideoCipher& operator=(const VideoCipher&)=delete;
    bool valid() const;
    bool seal(std::uint64_t seq,std::span<const std::uint8_t> aad,
              std::span<const std::uint8_t> plaintext,std::span<std::uint8_t> output,
              std::size_t &output_size);
    bool open(std::uint64_t seq,std::span<const std::uint8_t> aad,
              std::span<const std::uint8_t> ciphertext_tag,std::span<std::uint8_t> output,
              std::size_t &output_size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
