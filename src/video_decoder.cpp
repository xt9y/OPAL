#include <opal/video_decoder.hpp>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace opal {

struct VideoDecoder::Impl {
    AVCodecContext *ctx=nullptr;
};

VideoDecoder::VideoDecoder():impl_(new Impl){}

bool VideoDecoder::configure_h264(std::span<const std::uint8_t> extradata){
    if(!impl_)return false;
    if(impl_->ctx)avcodec_free_context(&impl_->ctx);
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    if(!codec)return false;
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

bool VideoDecoder::decode(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                          std::vector<DecodedVideoFrame>& out){
    if(!impl_||!impl_->ctx||unit.empty())return false;
    AVPacket *packet=av_packet_alloc();
    if(!packet)return false;
    if(av_new_packet(packet,static_cast<int>(unit.size()))<0){av_packet_free(&packet);return false;}
    std::memcpy(packet->data,unit.data(),unit.size());
    packet->pts=pts_us;
    packet->dts=pts_us;
    int rc=avcodec_send_packet(impl_->ctx,packet);
    av_packet_free(&packet);
    if(rc<0)return false;
    for(;;){
        AVFrame *frame=av_frame_alloc();
        if(!frame)return false;
        rc=avcodec_receive_frame(impl_->ctx,frame);
        if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF){av_frame_free(&frame);break;}
        if(rc<0){av_frame_free(&frame);return false;}
        out.push_back({frame,pts_us});
    }
    return true;
}

void VideoDecoder::flush(){if(impl_&&impl_->ctx)avcodec_flush_buffers(impl_->ctx);}

VideoDecoder::~VideoDecoder(){
    if(impl_){if(impl_->ctx)avcodec_free_context(&impl_->ctx);delete impl_;impl_=nullptr;}
}

}
