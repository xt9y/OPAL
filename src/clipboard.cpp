#include <opal/clipboard.hpp>

#include <algorithm>
#include <charconv>
#include <limits>

namespace opal { namespace {

bool valid_clipboard_text(std::string_view text){
    if(text.find('\0')!=std::string_view::npos)return false;
    const auto* p=reinterpret_cast<const unsigned char*>(text.data());
    std::size_t i=0;
    while(i<text.size()){
        const unsigned char c=p[i];
        if(c<=0x7f){++i;continue;}
        if(c>=0xc2&&c<=0xdf){
            if(i+1>=text.size()||(p[i+1]&0xc0)!=0x80)return false;
            i+=2;continue;
        }
        if(c==0xe0){
            if(i+2>=text.size()||p[i+1]<0xa0||p[i+1]>0xbf||(p[i+2]&0xc0)!=0x80)return false;
            i+=3;continue;
        }
        if((c>=0xe1&&c<=0xec)||(c>=0xee&&c<=0xef)){
            if(i+2>=text.size()||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80)return false;
            i+=3;continue;
        }
        if(c==0xed){
            if(i+2>=text.size()||p[i+1]<0x80||p[i+1]>0x9f||(p[i+2]&0xc0)!=0x80)return false;
            i+=3;continue;
        }
        if(c==0xf0){
            if(i+3>=text.size()||p[i+1]<0x90||p[i+1]>0xbf||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return false;
            i+=4;continue;
        }
        if(c>=0xf1&&c<=0xf3){
            if(i+3>=text.size()||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return false;
            i+=4;continue;
        }
        if(c==0xf4){
            if(i+3>=text.size()||p[i+1]<0x80||p[i+1]>0x8f||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return false;
            i+=4;continue;
        }
        return false;
    }
    return true;
}

bool parse_u64(std::string_view field,std::uint64_t& value){
    if(field.empty())return false;
    value=0;const auto* begin=field.data();const auto* end=begin+field.size();
    const auto result=std::from_chars(begin,end,value);
    return result.ec==std::errc{}&&result.ptr==end;
}

bool parse_size(std::string_view field,std::size_t& value){
    std::uint64_t parsed=0;if(!parse_u64(field,parsed)||parsed>std::numeric_limits<std::size_t>::max())return false;
    value=static_cast<std::size_t>(parsed);return true;
}

std::string header(std::uint64_t id,std::size_t offset,std::size_t total){
    return "CLIP "+std::to_string(id)+" "+std::to_string(offset)+" "+std::to_string(total)+" ";
}

}

void ClipboardSender::prime_local(std::string_view text){
    last_local_.assign(text);primed_=true;transfer_text_.clear();transfer_active_=false;outbound_.clear();
}

bool ClipboardSender::observe_local(std::string_view text){
    if(primed_&&text==last_local_)return false;
    last_local_.assign(text);primed_=true;transfer_text_.clear();transfer_active_=false;outbound_.clear();
    if(text.size()>kClipboardMaxBytes||!valid_clipboard_text(text))return false;
    transfer_text_=last_local_;transfer_active_=true;queue_current();return true;
}

void ClipboardSender::note_remote_applied(std::string_view text){
    last_local_.assign(text);primed_=true;transfer_text_.clear();transfer_active_=false;outbound_.clear();
}

void ClipboardSender::restart_transport(){
    if(!transfer_active_)return;
    outbound_.clear();queue_current();
}

void ClipboardSender::queue_current(){
    if(next_transfer_id_==0)next_transfer_id_=1;
    const auto id=next_transfer_id_++;
    if(transfer_text_.empty()){outbound_.push_back(header(id,0,0));return;}
    for(std::size_t offset=0;offset<transfer_text_.size();offset+=kClipboardChunkBytes){
        const auto count=std::min(kClipboardChunkBytes,transfer_text_.size()-offset);
        auto message=header(id,offset,transfer_text_.size());message.append(transfer_text_,offset,count);
        if(message.size()>kClipboardControlMaxBytes){outbound_.clear();transfer_text_.clear();transfer_active_=false;return;}
        outbound_.push_back(std::move(message));
    }
}

const std::string* ClipboardSender::next_message()const{return outbound_.empty()?nullptr:&outbound_.front();}
void ClipboardSender::pop_message(){if(outbound_.empty())return;outbound_.pop_front();if(outbound_.empty()){transfer_text_.clear();transfer_active_=false;}}
std::size_t ClipboardSender::queued_messages()const{return outbound_.size();}

ClipboardReceiveStatus ClipboardReceiver::receive(std::string_view message,std::string& completed){
    if(message.rfind("CLIP ",0)!=0)return ClipboardReceiveStatus::NotClipboard;
    std::size_t cursor=5;const auto id_end=message.find(' ',cursor);if(id_end==std::string_view::npos){reset();return ClipboardReceiveStatus::Rejected;}
    const auto offset_end=message.find(' ',id_end+1);if(offset_end==std::string_view::npos){reset();return ClipboardReceiveStatus::Rejected;}
    const auto total_end=message.find(' ',offset_end+1);if(total_end==std::string_view::npos){reset();return ClipboardReceiveStatus::Rejected;}
    std::uint64_t id=0;std::size_t offset=0,total=0;
    if(!parse_u64(message.substr(cursor,id_end-cursor),id)||id==0||
       !parse_size(message.substr(id_end+1,offset_end-id_end-1),offset)||
       !parse_size(message.substr(offset_end+1,total_end-offset_end-1),total)||total>kClipboardMaxBytes){reset();return ClipboardReceiveStatus::Rejected;}
    const auto chunk=message.substr(total_end+1);
    if(chunk.size()>kClipboardChunkBytes||offset>total||chunk.size()>total-offset||
       (total>offset&&chunk.empty())){reset();return ClipboardReceiveStatus::Rejected;}
    if(id!=transfer_id_){
        if(offset!=0){reset();return ClipboardReceiveStatus::Rejected;}
        transfer_id_=id;total_=total;buffer_.clear();buffer_.reserve(total);
    }else if(total!=total_||offset!=buffer_.size()){
        reset();return ClipboardReceiveStatus::Rejected;
    }
    if(offset!=buffer_.size()){reset();return ClipboardReceiveStatus::Rejected;}
    buffer_.append(chunk.data(),chunk.size());
    if(buffer_.size()<total_)return ClipboardReceiveStatus::Accepted;
    if(buffer_.size()!=total_||!valid_clipboard_text(buffer_)){reset();return ClipboardReceiveStatus::Rejected;}
    completed=buffer_;reset();return ClipboardReceiveStatus::Complete;
}

void ClipboardReceiver::reset(){transfer_id_=0;total_=0;buffer_.clear();}

}
