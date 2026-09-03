#include <opal/reliable_control.hpp>

#include <algorithm>

namespace opal {

std::uint64_t ReliableControlSender::enqueue(std::string payload,std::uint64_t now_ms){if(payload.empty()||payload.size()>kReliableControlMaxPayload||pending_.size()>=kReliableControlMaxPending||next_sequence_==0)return 0;const auto sequence=next_sequence_++;pending_.emplace(sequence,Pending{std::move(payload),now_ms,0});return sequence;}

std::vector<ReliableTransmission> ReliableControlSender::due(std::uint64_t now_ms){
    std::vector<ReliableTransmission> out;if(failed_)return out;out.reserve(pending_.size());
    for(auto &[sequence,pending]:pending_){if(now_ms<pending.next_send_ms)continue;if(pending.attempt>=8){failed_=true;out.clear();return out;}++pending.attempt;out.push_back({sequence,pending.payload,pending.attempt});const std::uint64_t rto=std::min<std::uint64_t>(500,60ULL<<(std::min<std::uint32_t>(pending.attempt-1,3)));pending.next_send_ms=now_ms+rto;}
    return out;
}

void ReliableControlSender::acknowledge(ReliableAckState ack){if(ack.sequence==0)return;pending_.erase(ack.sequence);for(std::uint32_t bit=0;bit<32;++bit)if((ack.bits>>bit)&1u){const std::uint64_t distance=static_cast<std::uint64_t>(bit)+1;if(ack.sequence>distance)pending_.erase(ack.sequence-distance);}}
std::size_t ReliableControlSender::pending()const{return pending_.size();}
bool ReliableControlSender::failed()const{return failed_;}
void ReliableControlSender::reset(){pending_.clear();next_sequence_=1;failed_=false;}

void ReliableControlReceiver::note_seen(std::uint64_t sequence){
    if(highest_seen_==0){highest_seen_=sequence;seen_bits_=0;return;}
    if(sequence>highest_seen_){const std::uint64_t delta=sequence-highest_seen_;if(delta>32)seen_bits_=0;else{seen_bits_<<=static_cast<std::uint32_t>(delta);seen_bits_|=1u<<static_cast<std::uint32_t>(delta-1);}highest_seen_=sequence;return;}
    const std::uint64_t age=highest_seen_-sequence;if(age>=1&&age<=32)seen_bits_|=1u<<static_cast<std::uint32_t>(age-1);
}

bool ReliableControlReceiver::receive(std::uint64_t sequence,std::string payload,std::vector<std::string>&delivered){
    if(sequence==0||payload.empty()||payload.size()>kReliableControlMaxPayload)return false;if(sequence<next_expected_){note_seen(sequence);return false;}if(buffered_.count(sequence)){note_seen(sequence);return false;}if(buffered_.size()>=kReliableControlMaxPending)return false;note_seen(sequence);buffered_.emplace(sequence,std::move(payload));for(;;){auto it=buffered_.find(next_expected_);if(it==buffered_.end())break;delivered.push_back(std::move(it->second));buffered_.erase(it);if(++next_expected_==0){reset();break;}}return true;
}
ReliableAckState ReliableControlReceiver::ack_state()const{return{highest_seen_,seen_bits_};}
std::size_t ReliableControlReceiver::buffered()const{return buffered_.size();}
void ReliableControlReceiver::reset(){buffered_.clear();next_expected_=1;highest_seen_=0;seen_bits_=0;}

bool LatestPointerReceiver::accept(std::uint64_t sequence,std::string payload){if(sequence==0||payload.empty()||payload.size()>kReliableControlMaxPayload||sequence<=sequence_)return false;sequence_=sequence;latest_=std::move(payload);return true;}
const std::string&LatestPointerReceiver::latest()const{return latest_;}
std::uint64_t LatestPointerReceiver::sequence()const{return sequence_;}
void LatestPointerReceiver::reset(){sequence_=0;latest_.clear();}

}
