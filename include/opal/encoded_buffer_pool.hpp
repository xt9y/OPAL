#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace opal {

class EncodedBufferPool {
public:
    static constexpr std::size_t kSlots=8;
    static constexpr std::size_t kMaxRetainedBufferBytes=4u*1024u*1024u;
    static constexpr std::size_t kMaxRetainedBytes=8u*1024u*1024u;

    std::vector<std::uint8_t> acquire(std::size_t required_capacity,std::size_t logical_size){
        std::vector<std::uint8_t> result;
        {
            std::lock_guard<std::mutex>lock(mu_);
            std::size_t best=kSlots;
            for(std::size_t i=0;i<kSlots;++i){
                if(!occupied_[i]||buffers_[i].capacity()<required_capacity)continue;
                if(best==kSlots||buffers_[i].capacity()<buffers_[best].capacity())best=i;
            }
            if(best!=kSlots){cached_bytes_-=buffers_[best].capacity();result=std::move(buffers_[best]);occupied_[best]=false;}
        }
        if(result.capacity()<required_capacity)result.reserve(required_capacity);
        result.resize(logical_size);
        return result;
    }

    void release(std::vector<std::uint8_t>buffer){
        const auto capacity=buffer.capacity();
        if(capacity==0||capacity>kMaxRetainedBufferBytes)return;
        std::lock_guard<std::mutex>lock(mu_);
        if(cached_bytes_+capacity>kMaxRetainedBytes)return;
        for(std::size_t i=0;i<kSlots;++i){if(occupied_[i])continue;buffers_[i]=std::move(buffer);occupied_[i]=true;cached_bytes_+=capacity;return;}
    }

    std::size_t cached_buffers()const{std::lock_guard<std::mutex>lock(mu_);std::size_t count=0;for(bool occupied:occupied_)if(occupied)++count;return count;}
    std::size_t cached_bytes()const{std::lock_guard<std::mutex>lock(mu_);return cached_bytes_;}

private:
    mutable std::mutex mu_;
    std::array<std::vector<std::uint8_t>,kSlots>buffers_{};
    std::array<bool,kSlots>occupied_{};
    std::size_t cached_bytes_=0;
};

inline EncodedBufferPool& encoded_buffer_pool(){static EncodedBufferPool pool;return pool;}

}
