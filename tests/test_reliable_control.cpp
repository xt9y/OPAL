#include <opal/reliable_control.hpp>
#include <cassert>
#include <string>
#include <vector>

int main(){
    opal::ReliableControlSender sender;const auto s1=sender.enqueue("KEY 30 1",1000),s2=sender.enqueue("KEY 30 0",1000);assert(s1==1&&s2==2&&sender.pending()==2);
    auto due=sender.due(1000);assert(due.size()==2&&due[0].attempt==1&&due[1].attempt==1);
    assert(sender.due(1010).empty());

    opal::ReliableControlReceiver receiver;std::vector<std::string> delivered;
    assert(receiver.receive(2,"KEY 30 0",delivered));assert(delivered.empty()&&receiver.buffered()==1);
    auto ack=receiver.ack_state();assert(ack.sequence==2&&(ack.bits&1u)==0u);
    assert(receiver.receive(1,"KEY 30 1",delivered));assert(delivered.size()==2&&delivered[0]=="KEY 30 1"&&delivered[1]=="KEY 30 0"&&receiver.buffered()==0);
    ack=receiver.ack_state();assert(ack.sequence==2&&(ack.bits&1u)==1u);
    std::vector<std::string> duplicate;assert(!receiver.receive(1,"KEY 30 1",duplicate));assert(duplicate.empty());
    sender.acknowledge(ack);assert(sender.pending()==0&&!sender.failed());

    const auto third=sender.enqueue("BUTTON 1 1",2000);assert(third==3);assert(sender.due(2000).size()==1);assert(sender.due(2060).size()==1);assert(sender.pending()==1);

    opal::LatestPointerReceiver pointer;assert(pointer.accept(100,"POINTER 10 10"));assert(pointer.latest()=="POINTER 10 10");assert(!pointer.accept(99,"POINTER 1 1"));assert(pointer.latest()=="POINTER 10 10");assert(pointer.accept(101,"POINTER 20 20"));assert(pointer.sequence()==101);
    // Pointer progress is independent of an unacknowledged reliable command.
    assert(sender.pending()==1&&pointer.latest()=="POINTER 20 20");

    opal::ReliableControlSender bounded;for(std::size_t i=0;i<opal::kReliableControlMaxPending;++i)assert(bounded.enqueue("KEY 1 1",0)!=0);assert(bounded.enqueue("KEY 1 1",0)==0);assert(bounded.enqueue(std::string(opal::kReliableControlMaxPayload+1,'X'),0)==0);
    sender.reset();receiver.reset();pointer.reset();assert(sender.pending()==0&&receiver.buffered()==0&&pointer.sequence()==0);
    return 0;
}
