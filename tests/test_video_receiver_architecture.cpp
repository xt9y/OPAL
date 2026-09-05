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
    assert(backlog.size()==2);
    auto first=backlog.pop();auto second=backlog.pop();
    assert(first&&*first=="p1");
    assert(second&&*second=="p2");
    assert(!backlog.pop());

    assert(backlog.push("old1",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.push("old2",false)==opal::VideoBacklogPush::Queued);
    assert(backlog.push("idr",true)==opal::VideoBacklogPush::ReplacedBacklog);
    assert(backlog.size()==1);
    auto idr=backlog.pop();
    assert(idr&&*idr=="idr");
    assert(backlog.size()==0);

    return 0;
}
