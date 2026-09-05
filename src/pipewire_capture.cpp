#include <opal/pipewire_capture.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OPAL_HAVE_NATIVE_PIPEWIRE
#define OPAL_HAVE_NATIVE_PIPEWIRE 0
#endif

#if OPAL_HAVE_NATIVE_PIPEWIRE
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <sys/mman.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
}

struct NativePipeWireVideoCapture::Impl {
    mutable std::mutex mu;
    std::condition_variable encoded_cv;
    std::deque<EncodedMediaUnit> encoded;
    MediaConfig config;
    std::uint64_t config_rev=0;
    std::string backend="unavailable",error;
    std::atomic<bool> run{false},terminal{false};
    std::thread setup_thread;

#if OPAL_HAVE_NATIVE_PIPEWIRE
    struct RawFrame {
        std::vector<std::uint8_t> pixels;
        int width=0,height=0,stride=0;
        AVPixelFormat format=AV_PIX_FMT_NONE;
        std::uint64_t capture_us=0;
        bool source_dmabuf=false;
    };
    struct EncoderState {
        AVCodecContext*ctx=nullptr;
        AVFrame*sw_frame=nullptr;
        AVFrame*hw_frame=nullptr;
        AVPacket*packet=nullptr;
        SwsContext*sws=nullptr;
        AVBufferRef*hw_device=nullptr;
        bool hardware=false;
        AVPixelFormat convert_format=AV_PIX_FMT_YUV420P;
        std::string name;
        void reset(){if(sws){sws_freeContext(sws);sws=nullptr;}if(packet)av_packet_free(&packet);if(sw_frame)av_frame_free(&sw_frame);if(hw_frame)av_frame_free(&hw_frame);if(ctx)avcodec_free_context(&ctx);av_buffer_unref(&hw_device);hardware=false;convert_format=AV_PIX_FMT_YUV420P;name.clear();}
        ~EncoderState(){reset();}
    };

    std::condition_variable raw_cv;
    std::deque<RawFrame> raw_frames;
    std::thread encoder_thread;
    std::mutex portal_mu;
    GCancellable*cancellable=nullptr;
    spa_video_info_raw raw_info{};
    int bitrate_kbps=0,fps=60,preferred_width=1920,preferred_height=1080;
    std::string token_file;
    pw_stream*stream=nullptr;
    std::atomic<bool>saw_dmabuf{false};

    struct PortalWait {GMainLoop*loop=nullptr;XdpSession*session=nullptr;GError*error=nullptr;bool ok=false;};
    static void created_cb(GObject*source,GAsyncResult*result,gpointer data){auto*s=static_cast<PortalWait*>(data);s->session=xdp_portal_create_screencast_session_finish(XDP_PORTAL(source),result,&s->error);g_main_loop_quit(s->loop);}
    static void started_cb(GObject*source,GAsyncResult*result,gpointer data){auto*s=static_cast<PortalWait*>(data);s->ok=xdp_session_start_finish(XDP_SESSION(source),result,&s->error);g_main_loop_quit(s->loop);}
    static AVPixelFormat av_format(std::uint32_t format){switch(format){case SPA_VIDEO_FORMAT_BGRA:return AV_PIX_FMT_BGRA;case SPA_VIDEO_FORMAT_BGRx:return AV_PIX_FMT_BGR0;case SPA_VIDEO_FORMAT_RGBA:return AV_PIX_FMT_RGBA;case SPA_VIDEO_FORMAT_RGBx:return AV_PIX_FMT_RGB0;default:return AV_PIX_FMT_NONE;}}
    static void stream_state_changed(void*data,pw_stream_state,pw_stream_state state,const char*message){auto*self=static_cast<Impl*>(data);if(state!=PW_STREAM_STATE_ERROR&&state!=PW_STREAM_STATE_UNCONNECTED)return;if(!self->run.load())return;self->set_error(message&&*message?message:"PipeWire stream disconnected");}
    static void stream_param_changed(void*data,std::uint32_t id,const spa_pod*param){auto*self=static_cast<Impl*>(data);if(id!=SPA_PARAM_Format||!param)return;spa_video_info_raw info{};if(spa_format_video_raw_parse(param,&info)>=0)self->raw_info=info;}
    static void stream_process(void*data){static_cast<Impl*>(data)->capture_latest_buffer();}
    static pw_stream_events make_stream_events(){pw_stream_events e{};e.version=PW_VERSION_STREAM_EVENTS;e.state_changed=&Impl::stream_state_changed;e.param_changed=&Impl::stream_param_changed;e.process=&Impl::stream_process;return e;}

    void set_error(std::string text){std::lock_guard<std::mutex>lock(mu);if(error.empty())error=std::move(text);terminal.store(true);encoded_cv.notify_all();raw_cv.notify_all();}
    void clear_cancellable(GCancellable*value){std::lock_guard<std::mutex>lock(portal_mu);if(cancellable==value)cancellable=nullptr;}
    std::string read_token()const{std::ifstream in(token_file);std::string token;if(in)std::getline(in,token);return token;}
    void save_token(XdpSession*session){char*token=xdp_session_get_restore_token(session);if(!token||!*token){g_free(token);return;}std::error_code ec;const auto path=std::filesystem::path(token_file);if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),ec);std::ofstream out(token_file,std::ios::trunc);if(out)out<<token<<'\n';g_free(token);}
    std::uint64_t capture_cycle_us(const pw_buffer*buffer)const{const auto local_now=monotonic_us();if(!stream||!buffer||!buffer->time)return local_now;const auto pipewire_now=pw_stream_get_nsec(stream);if(pipewire_now<buffer->time)return local_now;const auto age_us=(pipewire_now-buffer->time)/1000u;if(age_us>5000000u||age_us>local_now)return local_now;return local_now-age_us;}

    void capture_latest_buffer(){
        if(!stream||!run.load())return;
        pw_buffer*selected=nullptr;
        for(;;){pw_buffer*next=pw_stream_dequeue_buffer(stream);if(!next)break;if(selected)pw_stream_queue_buffer(stream,selected);selected=next;}
        if(!selected)return;
        auto requeue=[&]{pw_stream_queue_buffer(stream,selected);};
        spa_buffer*buffer=selected->buffer;if(!buffer||buffer->n_datas==0||raw_info.size.width==0||raw_info.size.height==0){requeue();return;}
        spa_data&d=buffer->datas[0];if(!d.chunk||d.chunk->stride<=0){requeue();return;}
        const AVPixelFormat format=av_format(raw_info.format);if(format==AV_PIX_FMT_NONE){requeue();return;}
        const int width=static_cast<int>(raw_info.size.width),height=static_cast<int>(raw_info.size.height),source_stride=d.chunk->stride;
        const std::size_t row_bytes=static_cast<std::size_t>(width)*4u;if(source_stride<static_cast<int>(row_bytes)){requeue();return;}
        const std::uint64_t end=static_cast<std::uint64_t>(d.chunk->offset)+static_cast<std::uint64_t>(height-1)*static_cast<std::uint64_t>(source_stride)+row_bytes;if(end>d.maxsize){requeue();return;}
        void*mapping=nullptr;const std::uint8_t*base=nullptr;if(d.data)base=static_cast<const std::uint8_t*>(d.data);else if((d.type==SPA_DATA_MemFd||d.type==SPA_DATA_DmaBuf)&&d.fd>=0){mapping=mmap(nullptr,d.maxsize,PROT_READ,MAP_PRIVATE,static_cast<int>(d.fd),d.mapoffset);if(mapping!=MAP_FAILED)base=static_cast<const std::uint8_t*>(mapping);else mapping=nullptr;}if(!base){requeue();return;}
        RawFrame frame;{std::lock_guard<std::mutex>lock(mu);if(raw_frames.size()>=2){frame.pixels=std::move(raw_frames.front().pixels);raw_frames.pop_front();}}
        frame.pixels.resize(row_bytes*static_cast<std::size_t>(height));frame.width=width;frame.height=height;frame.stride=static_cast<int>(row_bytes);frame.format=format;frame.capture_us=capture_cycle_us(selected);frame.source_dmabuf=d.type==SPA_DATA_DmaBuf;
        const auto*src=base+d.chunk->offset;for(int y=0;y<height;++y)std::copy_n(src+static_cast<std::ptrdiff_t>(y)*source_stride,row_bytes,frame.pixels.data()+static_cast<std::size_t>(y)*row_bytes);
        if(mapping)munmap(mapping,d.maxsize);requeue();if(frame.source_dmabuf)saw_dmabuf.store(true);
        {std::lock_guard<std::mutex>lock(mu);if(raw_frames.size()>=2)raw_frames.pop_front();raw_frames.push_back(std::move(frame));}raw_cv.notify_one();
    }

    void configure_common(AVCodecContext*ctx,const RawFrame&raw,AVPixelFormat format){ctx->width=raw.width;ctx->height=raw.height;ctx->pix_fmt=format;ctx->time_base=AVRational{1,std::max(15,fps)};ctx->framerate=AVRational{std::max(15,fps),1};ctx->bit_rate=static_cast<std::int64_t>(std::max(1000,bitrate_kbps))*1000;ctx->gop_size=normal_gop_frames(fps);ctx->max_b_frames=0;ctx->thread_count=1;ctx->flags|=AV_CODEC_FLAG_LOW_DELAY|AV_CODEC_FLAG_GLOBAL_HEADER;}
    bool allocate_sw_frame(EncoderState&enc,const RawFrame&raw){enc.sw_frame=av_frame_alloc();enc.packet=av_packet_alloc();if(!enc.sw_frame||!enc.packet)return false;enc.sw_frame->format=enc.convert_format;enc.sw_frame->width=raw.width;enc.sw_frame->height=raw.height;if(av_frame_get_buffer(enc.sw_frame,32)<0)return false;enc.sws=sws_getContext(raw.width,raw.height,raw.format,raw.width,raw.height,enc.convert_format,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);return enc.sws!=nullptr;}
    bool open_vaapi(EncoderState&enc,const RawFrame&raw){
        const AVCodec*codec=avcodec_find_encoder_by_name("h264_vaapi");if(!codec)return false;if(av_hwdevice_ctx_create(&enc.hw_device,AV_HWDEVICE_TYPE_VAAPI,nullptr,nullptr,0)<0||!enc.hw_device){enc.reset();return false;}enc.ctx=avcodec_alloc_context3(codec);if(!enc.ctx){enc.reset();return false;}enc.convert_format=AV_PIX_FMT_NV12;configure_common(enc.ctx,raw,AV_PIX_FMT_VAAPI);
        AVBufferRef*frames_ref=av_hwframe_ctx_alloc(enc.hw_device);if(!frames_ref){enc.reset();return false;}auto*frames=reinterpret_cast<AVHWFramesContext*>(frames_ref->data);frames->format=AV_PIX_FMT_VAAPI;frames->sw_format=AV_PIX_FMT_NV12;frames->width=raw.width;frames->height=raw.height;frames->initial_pool_size=4;if(av_hwframe_ctx_init(frames_ref)<0){av_buffer_unref(&frames_ref);enc.reset();return false;}enc.ctx->hw_frames_ctx=av_buffer_ref(frames_ref);av_buffer_unref(&frames_ref);if(!enc.ctx->hw_frames_ctx||avcodec_open2(enc.ctx,codec,nullptr)<0){enc.reset();return false;}enc.hw_frame=av_frame_alloc();if(!enc.hw_frame||!allocate_sw_frame(enc,raw)){enc.reset();return false;}enc.hardware=true;enc.name="h264_vaapi";return true;
    }
    static bool accepts_cpu_format(const AVCodec*codec,AVPixelFormat&format){
        if(!codec)return false;
        const AVPixelFormat*formats=nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61,12,100)
        int count=0;if(avcodec_get_supported_config(nullptr,codec,AV_CODEC_CONFIG_PIX_FORMAT,0,reinterpret_cast<const void**>(&formats),&count)<0)return false;if(!formats){format=AV_PIX_FMT_YUV420P;return true;}for(int i=0;i<count;++i)if(formats[i]==AV_PIX_FMT_NV12){format=AV_PIX_FMT_NV12;return true;}for(int i=0;i<count;++i)if(formats[i]==AV_PIX_FMT_YUV420P){format=AV_PIX_FMT_YUV420P;return true;}
#else
        formats=codec->pix_fmts;if(!formats){format=AV_PIX_FMT_YUV420P;return true;}for(const AVPixelFormat*p=formats;*p!=AV_PIX_FMT_NONE;++p)if(*p==AV_PIX_FMT_NV12){format=*p;return true;}for(const AVPixelFormat*p=formats;*p!=AV_PIX_FMT_NONE;++p)if(*p==AV_PIX_FMT_YUV420P){format=*p;return true;}
#endif
        return false;
    }
    bool open_cpu_input_encoder(EncoderState&enc,const RawFrame&raw){
        constexpr const char*candidates[]={"h264_v4l2m2m","h264_nvenc","h264_qsv","libx264","libopenh264"};
        for(const char*name:candidates){const AVCodec*codec=avcodec_find_encoder_by_name(name);AVPixelFormat format=AV_PIX_FMT_NONE;if(!accepts_cpu_format(codec,format))continue;enc.ctx=avcodec_alloc_context3(codec);if(!enc.ctx)continue;enc.convert_format=format;configure_common(enc.ctx,raw,format);AVDictionary*options=nullptr;if(std::string(name)=="libx264"){av_dict_set(&options,"preset","ultrafast",0);av_dict_set(&options,"tune","zerolatency",0);}const int rc=avcodec_open2(enc.ctx,codec,&options);av_dict_free(&options);if(rc<0){enc.reset();continue;}if(!allocate_sw_frame(enc,raw)){enc.reset();continue;}enc.hardware=std::string(name)!="libx264"&&std::string(name)!="libopenh264";enc.name=name;return true;}return false;
    }
    bool configure_encoder(EncoderState&enc,const RawFrame&raw){enc.reset();if(!open_vaapi(enc,raw)&&!open_cpu_input_encoder(enc,raw)){set_error("no usable in-process low-latency H.264 encoder");return false;}{std::lock_guard<std::mutex>lock(mu);backend=std::string("pipewire-native+")+enc.name+(enc.hardware?"+hw":"+sw")+(saw_dmabuf.load()?"+dmabuf-source":"");if(enc.ctx->extradata&&enc.ctx->extradata_size>0){config.kind=MediaKind::VideoH264;config.extradata.assign(enc.ctx->extradata,enc.ctx->extradata+enc.ctx->extradata_size);++config_rev;}}return true;}
    bool encode_one(EncoderState&enc,const RawFrame&raw,std::uint64_t frame_index){
        if(av_frame_make_writable(enc.sw_frame)<0)return false;const std::uint8_t*src[4]={raw.pixels.data(),nullptr,nullptr,nullptr};int src_stride[4]={raw.stride,0,0,0};if(sws_scale(enc.sws,src,src_stride,0,raw.height,enc.sw_frame->data,enc.sw_frame->linesize)<=0)return false;enc.sw_frame->pts=static_cast<std::int64_t>(frame_index);AVFrame*submit=enc.sw_frame;
        if(enc.ctx->pix_fmt==AV_PIX_FMT_VAAPI){av_frame_unref(enc.hw_frame);if(av_hwframe_get_buffer(enc.ctx->hw_frames_ctx,enc.hw_frame,0)<0||av_hwframe_transfer_data(enc.hw_frame,enc.sw_frame,0)<0)return false;enc.hw_frame->pts=enc.sw_frame->pts;submit=enc.hw_frame;}
        if(avcodec_send_frame(enc.ctx,submit)<0)return false;for(;;){const int rc=avcodec_receive_packet(enc.ctx,enc.packet);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;if(rc<0)return false;EncodedMediaUnit unit;unit.kind=MediaKind::VideoH264;unit.data.assign(enc.packet->data,enc.packet->data+enc.packet->size);unit.pts_us=static_cast<std::int64_t>(raw.capture_us);unit.capture_time_us=raw.capture_us;unit.keyframe=(enc.packet->flags&AV_PKT_FLAG_KEY)!=0;av_packet_unref(enc.packet);{std::lock_guard<std::mutex>lock(mu);if(config.extradata.empty()&&enc.ctx->extradata&&enc.ctx->extradata_size>0){config.kind=MediaKind::VideoH264;config.extradata.assign(enc.ctx->extradata,enc.ctx->extradata+enc.ctx->extradata_size);++config_rev;}if(encoded.size()>=2)encoded.pop_front();encoded.push_back(std::move(unit));}encoded_cv.notify_one();}return true;
    }
    void encoder_loop(){EncoderState enc;std::uint64_t frame_index=0;int width=0,height=0;AVPixelFormat input=AV_PIX_FMT_NONE;while(run.load()){RawFrame raw;{std::unique_lock<std::mutex>lock(mu);raw_cv.wait_for(lock,std::chrono::milliseconds(20),[&]{return !run.load()||!raw_frames.empty();});if(!run.load())break;if(raw_frames.empty())continue;raw=std::move(raw_frames.back());raw_frames.clear();}if(!enc.ctx||raw.width!=width||raw.height!=height||raw.format!=input){if(!configure_encoder(enc,raw))break;width=raw.width;height=raw.height;input=raw.format;frame_index=0;}if(!encode_one(enc,raw,frame_index++)){set_error("native H.264 encode failed");break;}}}

    bool open_portal(XdpPortal*&portal,XdpSession*&session,int&remote_fd,std::uint32_t&node_id){
        portal=xdp_portal_new();if(!portal){set_error("screencast portal unavailable");return false;}GMainLoop*loop=g_main_loop_new(nullptr,false);if(!loop){set_error("GLib main loop unavailable");return false;}GCancellable*cancel=g_cancellable_new();{std::lock_guard<std::mutex>lock(portal_mu);cancellable=cancel;}auto release_cancel=[&]{clear_cancellable(cancel);g_object_unref(cancel);};
        PortalWait wait;wait.loop=loop;const auto restore=read_token();xdp_portal_create_screencast_session(portal,XDP_OUTPUT_MONITOR,XDP_SCREENCAST_FLAG_NONE,XDP_CURSOR_MODE_EMBEDDED,XDP_PERSIST_MODE_PERSISTENT,restore.empty()?nullptr:restore.c_str(),cancel,&Impl::created_cb,&wait);g_main_loop_run(loop);if(!wait.session){if(wait.error){set_error(wait.error->message);g_error_free(wait.error);}g_main_loop_unref(loop);release_cancel();return false;}session=wait.session;wait={};wait.loop=loop;xdp_session_start(session,nullptr,cancel,&Impl::started_cb,&wait);g_main_loop_run(loop);if(!wait.ok){if(wait.error){set_error(wait.error->message);g_error_free(wait.error);}g_main_loop_unref(loop);release_cancel();return false;}GVariant*streams=xdp_session_get_streams(session);if(!streams||g_variant_n_children(streams)==0){if(streams)g_variant_unref(streams);set_error("portal returned no PipeWire stream");g_main_loop_unref(loop);release_cancel();return false;}GVariant*props=nullptr;g_variant_get_child(streams,0,"(u@a{sv})",&node_id,&props);if(props)g_variant_unref(props);g_variant_unref(streams);remote_fd=xdp_session_open_pipewire_remote(session);if(remote_fd<0){set_error("portal PipeWire remote unavailable");g_main_loop_unref(loop);release_cancel();return false;}save_token(session);g_main_loop_unref(loop);release_cancel();return true;
    }

    void setup_loop(){
        XdpPortal*portal=nullptr;XdpSession*session=nullptr;int remote_fd=-1;std::uint32_t node_id=PW_ID_ANY;if(!open_portal(portal,session,remote_fd,node_id)){if(session){xdp_session_close(session);g_object_unref(session);}if(portal)g_object_unref(portal);return;}
        pw_init(nullptr,nullptr);pw_thread_loop*loop=pw_thread_loop_new("opal-pipewire",nullptr);pw_context*context=loop?pw_context_new(pw_thread_loop_get_loop(loop),nullptr,0):nullptr;pw_core*core=context?pw_context_connect_fd(context,remote_fd,nullptr,0):nullptr;if(!loop||!context||!core){if(remote_fd>=0&&!core)close(remote_fd);set_error("PipeWire remote connection failed");if(core)pw_core_disconnect(core);if(context)pw_context_destroy(context);if(loop)pw_thread_loop_destroy(loop);xdp_session_close(session);g_object_unref(session);g_object_unref(portal);pw_deinit();return;}
        stream=pw_stream_new(core,"OPAL native capture",pw_properties_new(PW_KEY_MEDIA_TYPE,"Video",PW_KEY_MEDIA_CATEGORY,"Capture",PW_KEY_MEDIA_ROLE,"Screen",nullptr));if(!stream){set_error("PipeWire stream allocation failed");pw_core_disconnect(core);pw_context_destroy(context);pw_thread_loop_destroy(loop);xdp_session_close(session);g_object_unref(session);g_object_unref(portal);pw_deinit();return;}static const pw_stream_events events=make_stream_events();spa_hook listener{};pw_stream_add_listener(stream,&listener,&events,this);
        std::array<std::uint8_t,1024>pod_buffer{};spa_pod_builder builder=SPA_POD_BUILDER_INIT(pod_buffer.data(),pod_buffer.size());const spa_pod*params[1];spa_rectangle default_size{static_cast<std::uint32_t>(preferred_width),static_cast<std::uint32_t>(preferred_height)},min_size{16,16},max_size{7680,4320};spa_fraction default_rate{static_cast<std::uint32_t>(fps),1},min_rate{1,1},max_rate{240,1};params[0]=spa_pod_builder_add_object(&builder,SPA_TYPE_OBJECT_Format,SPA_PARAM_EnumFormat,SPA_FORMAT_mediaType,SPA_POD_Id(SPA_MEDIA_TYPE_video),SPA_FORMAT_mediaSubtype,SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),SPA_FORMAT_VIDEO_format,SPA_POD_CHOICE_ENUM_Id(5,SPA_VIDEO_FORMAT_BGRA,SPA_VIDEO_FORMAT_BGRA,SPA_VIDEO_FORMAT_BGRx,SPA_VIDEO_FORMAT_RGBA,SPA_VIDEO_FORMAT_RGBx),SPA_FORMAT_VIDEO_size,SPA_POD_CHOICE_RANGE_Rectangle(&default_size,&min_size,&max_size),SPA_FORMAT_VIDEO_framerate,SPA_POD_CHOICE_RANGE_Fraction(&default_rate,&min_rate,&max_rate));
        const int rc=pw_stream_connect(stream,PW_DIRECTION_INPUT,node_id,static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS),params,1);if(rc<0||pw_thread_loop_start(loop)<0){set_error("PipeWire stream connect failed");pw_stream_destroy(stream);stream=nullptr;pw_core_disconnect(core);pw_context_destroy(context);pw_thread_loop_destroy(loop);xdp_session_close(session);g_object_unref(session);g_object_unref(portal);pw_deinit();return;}
        {std::lock_guard<std::mutex>lock(mu);backend="pipewire-native+starting";}encoder_thread=std::thread([this]{encoder_loop();});while(run.load()&&!terminal.load())std::this_thread::sleep_for(std::chrono::milliseconds(20));run.store(false);raw_cv.notify_all();if(encoder_thread.joinable())encoder_thread.join();pw_thread_loop_stop(loop);pw_stream_destroy(stream);stream=nullptr;pw_core_disconnect(core);pw_context_destroy(context);pw_thread_loop_destroy(loop);xdp_session_close(session);g_object_unref(session);g_object_unref(portal);pw_deinit();terminal.store(true);encoded_cv.notify_all();
    }
#endif
};

NativePipeWireVideoCapture::NativePipeWireVideoCapture():impl_(std::make_unique<Impl>()){}
NativePipeWireVideoCapture::~NativePipeWireVideoCapture(){stop();}
bool NativePipeWireVideoCapture::compiled(){return OPAL_HAVE_NATIVE_PIPEWIRE!=0;}

bool NativePipeWireVideoCapture::start(const StreamOptions&stream,int bitrate_kbps,const std::string&restore_token_file){stop();impl_=std::make_unique<Impl>();
#if OPAL_HAVE_NATIVE_PIPEWIRE
    impl_->bitrate_kbps=std::max(1000,bitrate_kbps);impl_->fps=std::clamp(stream.fps,15,240);impl_->preferred_width=stream.max_width>0?std::clamp(stream.max_width,16,7680):1920;impl_->preferred_height=stream.max_height>0?std::clamp(stream.max_height,16,4320):1080;impl_->token_file=restore_token_file;impl_->run.store(true);impl_->terminal.store(false);impl_->backend="pipewire-native+initializing";impl_->setup_thread=std::thread([this]{impl_->setup_loop();});return true;
#else
    (void)stream;(void)bitrate_kbps;(void)restore_token_file;impl_->error="native PipeWire capture not compiled";impl_->terminal.store(true);return false;
#endif
}

bool NativePipeWireVideoCapture::next(EncodedMediaUnit&unit,int timeout_ms){if(!impl_)return false;std::unique_lock<std::mutex>lock(impl_->mu);impl_->encoded_cv.wait_for(lock,std::chrono::milliseconds(std::max(0,timeout_ms)),[&]{return !impl_->encoded.empty()||impl_->terminal.load()||!impl_->run.load();});if(impl_->encoded.empty())return false;unit=std::move(impl_->encoded.back());impl_->encoded.clear();return !unit.data.empty();}
bool NativePipeWireVideoCapture::ended()const{return !impl_||impl_->terminal.load();}
std::uint64_t NativePipeWireVideoCapture::config_revision()const{if(!impl_)return 0;std::lock_guard<std::mutex>lock(impl_->mu);return impl_->config_rev;}
MediaConfig NativePipeWireVideoCapture::config()const{if(!impl_)return{};std::lock_guard<std::mutex>lock(impl_->mu);return impl_->config;}
std::string NativePipeWireVideoCapture::backend_name()const{if(!impl_)return"unavailable";std::lock_guard<std::mutex>lock(impl_->mu);return impl_->backend;}
std::string NativePipeWireVideoCapture::last_error()const{if(!impl_)return"native capture unavailable";std::lock_guard<std::mutex>lock(impl_->mu);return impl_->error;}
void NativePipeWireVideoCapture::stop(){if(!impl_)return;impl_->run.store(false);impl_->encoded_cv.notify_all();
#if OPAL_HAVE_NATIVE_PIPEWIRE
    impl_->raw_cv.notify_all();{std::lock_guard<std::mutex>lock(impl_->portal_mu);if(impl_->cancellable)g_cancellable_cancel(impl_->cancellable);}if(impl_->setup_thread.joinable())impl_->setup_thread.join();if(impl_->encoder_thread.joinable())impl_->encoder_thread.join();
#endif
    std::lock_guard<std::mutex>lock(impl_->mu);impl_->encoded.clear();impl_->config={};impl_->config_rev=0;impl_->terminal.store(false);impl_->backend="unavailable";impl_->error.clear();}

}
