#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace opal {

struct LatencyPercentiles {
    double p50=0.0;
    double p95=0.0;
    double p99=0.0;
    std::size_t samples=0;
};

template<std::size_t Capacity=128>
class LatencyWindow {
    static_assert(Capacity>0);
public:
    void push(double value){
        if(value<0.0)return;
        samples_[cursor_]=value;
        cursor_=(cursor_+1)%Capacity;
        if(count_<Capacity)++count_;
    }

    void clear(){cursor_=0;count_=0;samples_.fill(0.0);}
    std::size_t size()const{return count_;}

    LatencyPercentiles snapshot()const{
        LatencyPercentiles result;result.samples=count_;if(count_==0)return result;
        std::array<double,Capacity> sorted{};
        std::copy_n(samples_.begin(),count_,sorted.begin());
        std::sort(sorted.begin(),sorted.begin()+static_cast<std::ptrdiff_t>(count_));
        auto percentile=[&](double q){
            const double position=q*static_cast<double>(count_-1);
            const auto lo=static_cast<std::size_t>(position);
            const auto hi=std::min(count_-1,lo+1);
            const double fraction=position-static_cast<double>(lo);
            return sorted[lo]+(sorted[hi]-sorted[lo])*fraction;
        };
        result.p50=percentile(0.50);result.p95=percentile(0.95);result.p99=percentile(0.99);return result;
    }
private:
    std::array<double,Capacity>samples_{};
    std::size_t cursor_=0,count_=0;
};

}
