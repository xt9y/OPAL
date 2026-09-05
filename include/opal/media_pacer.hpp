#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace opal {

constexpr std::uint64_t pacer_sleep_slice_us(std::uint64_t wait_us){return wait_us>250?250:wait_us;}

class MediaTokenBucket {
public:
    void reset(double capacity_bytes,std::uint64_t now_us){
        capacity_=std::max(0.0,capacity_bytes);
        tokens_=capacity_;
        last_us_=now_us;
    }

    void top_up(double capacity_bytes,std::uint64_t now_us){
        refill(capacity_bytes,1,now_us,false);
        capacity_=std::max(0.0,capacity_bytes);
        tokens_=capacity_;
        last_us_=now_us;
    }

    std::uint64_t reserve_or_delay_us(std::size_t bytes,double capacity_bytes,int pacing_kbps,std::uint64_t now_us){
        if(bytes==0)return 0;
        const double capacity=std::max(0.0,capacity_bytes);
        if(capacity<static_cast<double>(bytes)||pacing_kbps<=0)return UINT64_MAX;
        refill(capacity,pacing_kbps,now_us,true);
        if(tokens_>=static_cast<double>(bytes)){
            tokens_-=static_cast<double>(bytes);
            return 0;
        }
        const double bytes_per_us=std::max(0.001,static_cast<double>(pacing_kbps)/8000.0);
        return static_cast<std::uint64_t>(std::max(1.0,std::ceil((static_cast<double>(bytes)-tokens_)/bytes_per_us)));
    }

    double tokens()const{return tokens_;}

private:
    void refill(double capacity,int pacing_kbps,std::uint64_t now_us,bool add_tokens){
        if(last_us_==0){last_us_=now_us;capacity_=capacity;tokens_=std::min(tokens_,capacity);return;}
        if(now_us<last_us_)now_us=last_us_;
        if(add_tokens){
            const double bytes_per_us=std::max(0.001,static_cast<double>(pacing_kbps)/8000.0);
            tokens_=std::min(capacity,tokens_+static_cast<double>(now_us-last_us_)*bytes_per_us);
        }else tokens_=std::min(tokens_,capacity);
        capacity_=capacity;
        last_us_=now_us;
    }

    double capacity_=0.0;
    double tokens_=0.0;
    std::uint64_t last_us_=0;
};

}
