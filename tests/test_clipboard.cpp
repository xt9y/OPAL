#include <opal/clipboard.hpp>

#include <cassert>
#include <string>

int main(){
    opal::ClipboardSender sender;
    sender.prime_local("before");
    assert(!sender.observe_local("before"));
    assert(sender.observe_local("after"));
    assert(sender.queued_messages()>0);

    opal::ClipboardReceiver receiver;
    std::string completed;
    bool got=false;
    while(const auto* message=sender.next_message()){
        const auto status=receiver.receive(*message,completed);
        if(status==opal::ClipboardReceiveStatus::Complete)got=true;
        else assert(status==opal::ClipboardReceiveStatus::Accepted);
        sender.pop_message();
    }
    assert(got);
    assert(completed=="after");

    sender.note_remote_applied(completed);
    assert(!sender.observe_local("after"));

    assert(sender.observe_local(""));
    got=false;completed="sentinel";
    while(const auto* message=sender.next_message()){
        const auto status=receiver.receive(*message,completed);
        if(status==opal::ClipboardReceiveStatus::Complete)got=true;
        else assert(status==opal::ClipboardReceiveStatus::Accepted);
        sender.pop_message();
    }
    assert(got&&completed.empty());

    std::string large(opal::kClipboardMaxBytes,'x');
    assert(sender.observe_local(large));
    assert(sender.queued_messages()>1);
    got=false;completed.clear();
    while(const auto* message=sender.next_message()){
        assert(message->size()<=opal::kClipboardControlMaxBytes);
        const auto status=receiver.receive(*message,completed);
        if(status==opal::ClipboardReceiveStatus::Complete)got=true;
        else assert(status==opal::ClipboardReceiveStatus::Accepted);
        sender.pop_message();
    }
    assert(got&&completed==large);

    std::string too_large(opal::kClipboardMaxBytes+1,'y');
    assert(!sender.observe_local(too_large));
    assert(sender.queued_messages()==0);

    assert(sender.observe_local(large));
    assert(sender.next_message());
    sender.pop_message();
    sender.restart_transport();
    assert(sender.next_message());
    opal::ClipboardReceiver after_reconnect;
    got=false;completed.clear();
    while(const auto* message=sender.next_message()){
        const auto status=after_reconnect.receive(*message,completed);
        if(status==opal::ClipboardReceiveStatus::Complete)got=true;
        else assert(status==opal::ClipboardReceiveStatus::Accepted);
        sender.pop_message();
    }
    assert(got&&completed==large);

    completed.clear();
    assert(receiver.receive("CLIP nope",completed)==opal::ClipboardReceiveStatus::Rejected);
    assert(receiver.receive("KEY 30 1",completed)==opal::ClipboardReceiveStatus::NotClipboard);

    opal::ClipboardReceiver bad_receiver;
    std::string bad="CLIP 7 0 2 ";bad.push_back(static_cast<char>(0xc3));bad.push_back(static_cast<char>(0x28));
    assert(bad_receiver.receive(bad,completed)==opal::ClipboardReceiveStatus::Rejected);
    std::string nul="CLIP 8 0 1 ";nul.push_back('\0');
    assert(bad_receiver.receive(nul,completed)==opal::ClipboardReceiveStatus::Rejected);
    return 0;
}
