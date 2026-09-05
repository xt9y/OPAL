#include <opal/video_backlog.hpp>

#include <cassert>
#include <string>

int main(){
    opal::VideoBacklog<std::string,2> backlog;
    assert(backlog.size()==0);
    assert(backlog.push("p1",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.push("p2",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.size()==2);

    assert(backlog.push("p3",false)==opal::VideoBacklogPush::DroppedIncoming);
    assert(backlog.awaiting_keyframe());
    auto first=backlog.pop();assert(first&&*first=="p1");
    assert(backlog.push("dependent-p4",false)==opal::VideoBacklogPush::DroppedIncoming);
    auto second=backlog.pop();assert(second&&*second=="p2");
    assert(backlog.push("dependent-p5",false)==opal::VideoBacklogPush::DroppedIncoming);
    assert(!backlog.pop());

    assert(backlog.push("idr",true)==opal::VideoBacklogPush::ReplacedBacklog);
    assert(!backlog.awaiting_keyframe());
    auto idr=backlog.pop();assert(idr&&*idr=="idr");
    assert(backlog.push("after-idr",false)==opal::VideoBacklogPush::Queued);
    backlog.clear();
    assert(!backlog.awaiting_keyframe()&&backlog.empty());

    assert(backlog.push("old1",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.push("old2",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.push("new-idr",true)==opal::VideoBacklogPush::ReplacedBacklog);
    assert(backlog.size()==1);
    auto new_idr=backlog.pop();
    assert(new_idr&&*new_idr=="new-idr");
    assert(backlog.size()==0);

    return 0;
}
