#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace opal {

enum class VideoBacklogPush {
    Queued,
    DroppedIncoming,
    ReplacedBacklog
};

template <class T,std::size_t Capacity>
class VideoBacklog {
    static_assert(Capacity>0);
public:
    VideoBacklogPush push(T item,bool keyframe){
        const bool replaced=keyframe&&count_>0;
        if(keyframe)clear();
        if(count_>=Capacity)return VideoBacklogPush::DroppedIncoming;
        const auto tail=(head_+count_)%Capacity;
        slots_[tail]=std::move(item);
        ++count_;
        return replaced?VideoBacklogPush::ReplacedBacklog:VideoBacklogPush::Queued;
    }

    std::optional<T> pop(){
        if(count_==0)return std::nullopt;
        auto item=std::move(slots_[head_]);
        slots_[head_].reset();
        head_=(head_+1)%Capacity;
        --count_;
        return item;
    }

    void clear(){for(auto&slot:slots_)slot.reset();head_=0;count_=0;}
    std::size_t size()const{return count_;}
    bool empty()const{return count_==0;}
    static constexpr std::size_t capacity(){return Capacity;}

private:
    std::array<std::optional<T>,Capacity>slots_{};
    std::size_t head_=0;
    std::size_t count_=0;
};

}
