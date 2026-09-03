#include <opal/video_decoder.hpp>

#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace opal {

struct VideoDecoder::Impl {
    AVCodecContext *ctx=nullptr;
    AVPacket *packet=nullptr;
    AVFrame *scratch=nullptr;
    AVFrame *latest=nullptr;
    std::vector<std::uint8_t> packet_bytes;

    bool ensure_io(){
        if(!packet)packet=av_packet_alloc();
        if(!scratch)scratch=av_frame_alloc();
        if(!latest)latest=av_frame_alloc();
        return packet&&scratch&&latest;
    }

    void clear_latest(){if(latest)av_frame_unref(latest);}
};

VideoDecoder::VideoDecoder():impl_(new Impl){}

bool VideoDecoder::configure_h264(std::span<const std::uint8_t> extradata){
    if(!impl_)return false;
    if(impl_->ctx)avcodec_free_context(&impl_->ctx);
    impl_->clear_latest();
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!codec||!impl_->ensure_io())return false;
    impl_->ctx=avcodec_alloc_context3(codec);
    if(!impl_->ctx)return false;
    impl_->ctx->flags|=AV_CODEC_FLAG_LOW_DELAY;
    impl_->ctx->flags2|=AV_CODEC_FLAG2_FAST;
    impl_->ctx->thread_count=1;
    impl_->ctx->thread_type=0;
    impl_->ctx->pkt_timebase=AVRational{1,1000000};
    impl_->ctx->time_base=AVRational{1,1000000};
    if(!extradata.empty()){
        impl_->ctx->extradata=static_cast<std::uint8_t*>(
            av_mallocz(extradata.size()+AV_INPUT_BUFFER_PADDING_SIZE));
        if(!impl_->ctx->extradata){avcodec_free_context(&impl_->ctx);return false;}
        std::memcpy(impl_->ctx->extradata,extradata.data(),extradata.size());
        impl_->ctx->extradata_size=static_cast<int>(extradata.size());
    }
    if(avcodec_open2(impl_->ctx,codec,nullptr)<0){
        avcodec_free_context(&impl_->ctx);
        return false;
    }
    return true;
}

bool VideoDecoder::decode_latest(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                                 DecodedVideoView &out,std::size_t &superseded){
    out={};superseded=0;
    if(!impl_||!impl_->ctx||unit.empty()||!impl_->ensure_io())return false;

    const std::size_t needed=unit.size()+AV_INPUT_BUFFER_PADDING_SIZE;
    if(impl_->packet_bytes.size()<needed)impl_->packet_bytes.resize(needed);
    std::memcpy(impl_->packet_bytes.data(),unit.data(),unit.size());
    std::memset(impl_->packet_bytes.data()+unit.size(),0,AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(impl_->packet);
    impl_->packet->data=impl_->packet_bytes.data();
    impl_->packet->size=static_cast<int>(unit.size());
    impl_->packet->pts=pts_us;
    impl_->packet->dts=pts_us;
    const int send_rc=avcodec_send_packet(impl_->ctx,impl_->packet);
    impl_->packet->data=nullptr;
    impl_->packet->size=0;
    if(send_rc<0)return false;

    std::size_t decoded=0;
    for(;;){
        const int rc=avcodec_receive_frame(impl_->ctx,impl_->scratch);
        if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;
        if(rc<0)return false;
        av_frame_unref(impl_->latest);
        av_frame_move_ref(impl_->latest,impl_->scratch);
        ++decoded;
    }
    if(decoded==0)return true;
    superseded=decoded-1;
    out.frame=impl_->latest;
    out.pts_us=pts_us;
    return true;
}

bool VideoDecoder::decode(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                          std::vector<DecodedVideoFrame>& out){
    DecodedVideoView latest;std::size_t superseded=0;
    if(!decode_latest(unit,pts_us,latest,superseded))return false;
    if(!latest.frame)return true;
    AVFrame *owned=av_frame_clone(latest.frame);
    if(!owned)return false;
    out.push_back({owned,latest.pts_us});
    return true;
}

void VideoDecoder::flush(){
    if(!impl_)return;
    if(impl_->ctx)avcodec_flush_buffers(impl_->ctx);
    impl_->clear_latest();
    if(impl_->scratch)av_frame_unref(impl_->scratch);
    if(impl_->packet)av_packet_unref(impl_->packet);
}

VideoDecoder::~VideoDecoder(){
    if(impl_){
        if(impl_->ctx)avcodec_free_context(&impl_->ctx);
        if(impl_->packet)av_packet_free(&impl_->packet);
        if(impl_->scratch)av_frame_free(&impl_->scratch);
        if(impl_->latest)av_frame_free(&impl_->latest);
        delete impl_;impl_=nullptr;
    }
}

}
