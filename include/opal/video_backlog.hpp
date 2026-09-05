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
        if(keyframe){
            const bool replaced=count_>0||awaiting_keyframe_;
            clear_slots();
            awaiting_keyframe_=false;
            queue(std::move(item));
            return replaced?VideoBacklogPush::ReplacedBacklog:VideoBacklogPush::Queued;
        }
        if(awaiting_keyframe_)return VideoBacklogPush::DroppedIncoming;
        if(count_>=Capacity){awaiting_keyframe_=true;return VideoBacklogPush::DroppedIncoming;}
        queue(std::move(item));
        return VideoBacklogPush::Queued;
    }

    std::optional<T> pop(){
        if(count_==0)return std::nullopt;
        auto item=std::move(slots_[head_]);
        slots_[head_].reset();
        head_=(head_+1)%Capacity;
        --count_;
        return item;
    }

    void clear(){clear_slots();awaiting_keyframe_=false;}
    std::size_t size()const{return count_;}
    bool empty()const{return count_==0;}
    bool awaiting_keyframe()const{return awaiting_keyframe_;}
    static constexpr std::size_t capacity(){return Capacity;}

private:
    void queue(T item){const auto tail=(head_+count_)%Capacity;slots_[tail]=std::move(item);++count_;}
    void clear_slots(){for(auto&slot:slots_)slot.reset();head_=0;count_=0;}

    std::array<std::optional<T>,Capacity>slots_{};
    std::size_t head_=0;
    std::size_t count_=0;
    bool awaiting_keyframe_=false;
};

}
