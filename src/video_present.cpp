#define GL_GLEXT_PROTOTYPES 1
#include <opal/video_present.hpp>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace opal {
namespace {

GLuint compile_shader(GLenum type,const char *source){
    GLuint shader=glCreateShader(type);
    if(!shader)return 0;
    glShaderSource(shader,1,&source,nullptr);
    glCompileShader(shader);
    GLint ok=GL_FALSE;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok){glDeleteShader(shader);return 0;}
    return shader;
}

GLuint make_program(){
    static const char *vertex=R"GLSL(
#version 120
varying vec2 uv;
void main(){
    gl_Position=gl_Vertex;
    uv=gl_MultiTexCoord0.xy;
}
)GLSL";
    static const char *fragment=R"GLSL(
#version 120
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
uniform sampler2D tex_uv;
uniform int nv12;
varying vec2 uv;
void main(){
    float y=texture2D(tex_y,uv).r;
    float u;
    float v;
    if(nv12!=0){
        vec4 c=texture2D(tex_uv,uv);
        u=c.r-0.5;
        v=c.a-0.5;
    }else{
        u=texture2D(tex_u,uv).r-0.5;
        v=texture2D(tex_v,uv).r-0.5;
    }
    y=max(0.0,1.1643*(y-0.0625));
    gl_FragColor=vec4(
        y+1.5958*v,
        y-0.39173*u-0.81290*v,
        y+2.017*u,
        1.0);
}
)GLSL";
    GLuint vs=compile_shader(GL_VERTEX_SHADER,vertex),fs=compile_shader(GL_FRAGMENT_SHADER,fragment);
    if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);return 0;}
    GLuint program=glCreateProgram();
    if(!program){glDeleteShader(vs);glDeleteShader(fs);return 0;}
    glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    glDeleteShader(vs);glDeleteShader(fs);
    GLint ok=GL_FALSE;glGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok){glDeleteProgram(program);return 0;}
    return program;
}

const void *upload_source(const std::uint8_t *src,int stride,int width,int height,
                          int bytes_per_pixel,std::vector<std::uint8_t> &scratch,
                          int &row_length){
    row_length=0;
    if(!src||width<=0||height<=0||bytes_per_pixel<=0)return nullptr;
    const int row_bytes=width*bytes_per_pixel;
    if(stride>0&&stride>=row_bytes&&stride%bytes_per_pixel==0){
        row_length=stride/bytes_per_pixel;
        return src;
    }
    scratch.resize(static_cast<std::size_t>(row_bytes)*static_cast<std::size_t>(height));
    for(int y=0;y<height;++y)
        std::memcpy(scratch.data()+static_cast<std::size_t>(y)*row_bytes,
                    src+static_cast<std::ptrdiff_t>(y)*stride,row_bytes);
    return scratch.data();
}

void upload_plane(GLenum texture_unit,GLuint texture,GLenum format,int width,int height,
                  const std::uint8_t *src,int stride,int bytes_per_pixel,
                  std::vector<std::uint8_t> &scratch){
    int row_length=0;
    const void *pixels=upload_source(src,stride,width,height,bytes_per_pixel,scratch,row_length);
    if(!pixels)return;
    glActiveTexture(texture_unit);glBindTexture(GL_TEXTURE_2D,texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,row_length);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,width,height,format,GL_UNSIGNED_BYTE,pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
}

void request_fullscreen(Display *display,Window window){
    Atom state=XInternAtom(display,"_NET_WM_STATE",False);
    Atom fullscreen=XInternAtom(display,"_NET_WM_STATE_FULLSCREEN",False);
    if(state==None||fullscreen==None)return;
    XEvent event{};event.type=ClientMessage;event.xclient.window=window;event.xclient.message_type=state;event.xclient.format=32;
    event.xclient.data.l[0]=1;event.xclient.data.l[1]=static_cast<long>(fullscreen);
    XSendEvent(display,DefaultRootWindow(display),False,SubstructureRedirectMask|SubstructureNotifyMask,&event);
}

void request_compositor_bypass(Display *display,Window window){
    Atom bypass=XInternAtom(display,"_NET_WM_BYPASS_COMPOSITOR",False);
    if(bypass==None)return;
    unsigned long enabled=1;
    XChangeProperty(display,window,bypass,XA_CARDINAL,32,PropModeReplace,
                    reinterpret_cast<unsigned char*>(&enabled),1);
}

void disable_swap_interval(Display *display,GLXDrawable drawable){
    using SwapIntervalExt=void(*)(Display*,GLXDrawable,int);
    using SwapIntervalMesa=int(*)(unsigned int);
    auto ext=reinterpret_cast<SwapIntervalExt>(glXGetProcAddressARB(
        reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
    if(ext){ext(display,drawable,0);return;}
    auto mesa=reinterpret_cast<SwapIntervalMesa>(glXGetProcAddressARB(
        reinterpret_cast<const GLubyte*>("glXSwapIntervalMESA")));
    if(mesa)mesa(0);
}

void fitted_viewport(int window_width,int window_height,int source_width,int source_height){
    window_width=std::max(1,window_width);window_height=std::max(1,window_height);
    source_width=std::max(1,source_width);source_height=std::max(1,source_height);
    const long long lhs=static_cast<long long>(window_width)*source_height;
    const long long rhs=static_cast<long long>(window_height)*source_width;
    int width=window_width,height=window_height,x=0,y=0;
    if(lhs>rhs){width=std::max(1,static_cast<int>(static_cast<long long>(window_height)*source_width/source_height));x=(window_width-width)/2;}
    else if(lhs<rhs){height=std::max(1,static_cast<int>(static_cast<long long>(window_width)*source_height/source_width));y=(window_height-height)/2;}
    glViewport(x,y,width,height);
}

}

struct VideoPresenter::Impl {
    Display *display=nullptr;
    Window window=0;
    Colormap colormap=0;
    GLXContext context=nullptr;
    GLuint program=0;
    GLint nv12_uniform=-1;
    std::array<GLuint,4> textures{};
    int source_width=0,source_height=0;
    int texture_width=0,texture_height=0;
    int window_width=1,window_height=1;
    std::vector<std::uint8_t> scratch_y,scratch_u,scratch_v,scratch_uv;

    bool init_gl(){
        program=make_program();if(!program)return false;
        glGenTextures(static_cast<GLsizei>(textures.size()),textures.data());
        for(GLuint texture:textures){
            glBindTexture(GL_TEXTURE_2D,texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program,"tex_y"),0);
        glUniform1i(glGetUniformLocation(program,"tex_u"),1);
        glUniform1i(glGetUniformLocation(program,"tex_v"),2);
        glUniform1i(glGetUniformLocation(program,"tex_uv"),3);
        nv12_uniform=glGetUniformLocation(program,"nv12");
        glUseProgram(0);
        return nv12_uniform>=0;
    }

    void ensure_texture_storage(int width,int height){
        if(width==texture_width&&height==texture_height)return;
        texture_width=width;texture_height=height;
        const int cw=(width+1)/2,ch=(height+1)/2;
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[0]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,width,height,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,nullptr);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,textures[1]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,cw,ch,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,nullptr);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,textures[2]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,cw,ch,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,nullptr);
        glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,textures[3]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE_ALPHA,cw,ch,0,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,nullptr);
    }

    void consume_window_events(){
        if(!display)return;
        while(XPending(display)>0){
            XEvent event{};XNextEvent(display,&event);
            if(event.type==ConfigureNotify&&event.xconfigure.window==window){
                window_width=std::max(1,event.xconfigure.width);
                window_height=std::max(1,event.xconfigure.height);
            }
        }
    }
};

VideoPresenter::VideoPresenter():impl_(std::make_unique<Impl>()){}

bool VideoPresenter::open(int source_width,int source_height,bool fullscreen){
    close();
    if(source_width<=0||source_height<=0)return false;
    impl_=std::make_unique<Impl>();
    impl_->display=XOpenDisplay(nullptr);if(!impl_->display)return false;
    int screen=DefaultScreen(impl_->display);
    int attrs[]={GLX_RGBA,GLX_DOUBLEBUFFER,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,None};
    XVisualInfo *visual=glXChooseVisual(impl_->display,screen,attrs);
    if(!visual){close();return false;}
    impl_->colormap=XCreateColormap(impl_->display,RootWindow(impl_->display,screen),visual->visual,AllocNone);
    XSetWindowAttributes swa{};swa.colormap=impl_->colormap;swa.event_mask=StructureNotifyMask|ExposureMask|KeyPressMask|KeyReleaseMask|PointerMotionMask|ButtonPressMask|ButtonReleaseMask;
    unsigned width=fullscreen?static_cast<unsigned>(DisplayWidth(impl_->display,screen)):static_cast<unsigned>(source_width);
    unsigned height=fullscreen?static_cast<unsigned>(DisplayHeight(impl_->display,screen)):static_cast<unsigned>(source_height);
    impl_->window_width=static_cast<int>(width);impl_->window_height=static_cast<int>(height);
    impl_->window=XCreateWindow(impl_->display,RootWindow(impl_->display,screen),0,0,width,height,0,visual->depth,InputOutput,visual->visual,CWColormap|CWEventMask,&swa);
    if(impl_->window)impl_->context=glXCreateContext(impl_->display,visual,nullptr,True);
    XFree(visual);
    if(!impl_->window||!impl_->context){close();return false;}
    XStoreName(impl_->display,impl_->window,"OPAL");
    request_compositor_bypass(impl_->display,impl_->window);
    XMapRaised(impl_->display,impl_->window);XFlush(impl_->display);
    if(fullscreen)request_fullscreen(impl_->display,impl_->window);
    if(!glXMakeCurrent(impl_->display,impl_->window,impl_->context)){close();return false;}
    disable_swap_interval(impl_->display,impl_->window);
    impl_->source_width=source_width;impl_->source_height=source_height;
    if(!impl_->init_gl()){close();return false;}
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glClearColor(0.f,0.f,0.f,1.f);
    return true;
}

bool VideoPresenter::present(DecodedVideoFrame decoded){
    AVFrame *frame=decoded.frame;
    if(!frame)return false;
    auto release=[&]{av_frame_free(&frame);};
    if(!impl_||!impl_->display||!impl_->window||!impl_->context){release();return false;}
    if(frame->width<=0||frame->height<=0){release();return false;}
    const bool yuv420=frame->format==AV_PIX_FMT_YUV420P||frame->format==AV_PIX_FMT_YUVJ420P;
    const bool nv12=frame->format==AV_PIX_FMT_NV12;
    if(!yuv420&&!nv12){release();return false;}
    if(!glXMakeCurrent(impl_->display,impl_->window,impl_->context)){release();return false;}
    impl_->consume_window_events();
    glViewport(0,0,impl_->window_width,impl_->window_height);glClear(GL_COLOR_BUFFER_BIT);
    fitted_viewport(impl_->window_width,impl_->window_height,frame->width,frame->height);
    impl_->ensure_texture_storage(frame->width,frame->height);
    upload_plane(GL_TEXTURE0,impl_->textures[0],GL_LUMINANCE,frame->width,frame->height,
                 frame->data[0],frame->linesize[0],1,impl_->scratch_y);
    if(yuv420){
        const int cw=(frame->width+1)/2,ch=(frame->height+1)/2;
        upload_plane(GL_TEXTURE1,impl_->textures[1],GL_LUMINANCE,cw,ch,
                     frame->data[1],frame->linesize[1],1,impl_->scratch_u);
        upload_plane(GL_TEXTURE2,impl_->textures[2],GL_LUMINANCE,cw,ch,
                     frame->data[2],frame->linesize[2],1,impl_->scratch_v);
    }else{
        const int cw=(frame->width+1)/2,ch=(frame->height+1)/2;
        upload_plane(GL_TEXTURE3,impl_->textures[3],GL_LUMINANCE_ALPHA,cw,ch,
                     frame->data[1],frame->linesize[1],2,impl_->scratch_uv);
    }
    glUseProgram(impl_->program);glUniform1i(impl_->nv12_uniform,nv12?1:0);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.f,1.f);glVertex2f(-1.f,-1.f);
    glTexCoord2f(1.f,1.f);glVertex2f(1.f,-1.f);
    glTexCoord2f(0.f,0.f);glVertex2f(-1.f,1.f);
    glTexCoord2f(1.f,0.f);glVertex2f(1.f,1.f);
    glEnd();
    glUseProgram(0);glXSwapBuffers(impl_->display,impl_->window);glFlush();
    release();return true;
}

Window VideoPresenter::x11_window() const{return impl_?impl_->window:0;}

std::pair<int,int> VideoPresenter::drawable_size() const{
    if(!impl_||!impl_->display||!impl_->window)return{0,0};
    XWindowAttributes wa{};if(!XGetWindowAttributes(impl_->display,impl_->window,&wa))return{0,0};return{wa.width,wa.height};
}

std::size_t VideoPresenter::pending_frame_count() const{return 0;}

void VideoPresenter::close(){
    if(!impl_)return;
    if(impl_->display&&impl_->context&&impl_->window)glXMakeCurrent(impl_->display,impl_->window,impl_->context);
    if(impl_->program){glDeleteProgram(impl_->program);impl_->program=0;}
    if(impl_->textures[0]){glDeleteTextures(static_cast<GLsizei>(impl_->textures.size()),impl_->textures.data());impl_->textures.fill(0);}
    if(impl_->display&&impl_->context){glXMakeCurrent(impl_->display,None,nullptr);glXDestroyContext(impl_->display,impl_->context);impl_->context=nullptr;}
    if(impl_->display&&impl_->window){XDestroyWindow(impl_->display,impl_->window);impl_->window=0;}
    if(impl_->display&&impl_->colormap){XFreeColormap(impl_->display,impl_->colormap);impl_->colormap=0;}
    if(impl_->display){XCloseDisplay(impl_->display);impl_->display=nullptr;}
}

VideoPresenter::~VideoPresenter(){close();}

}
