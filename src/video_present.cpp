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

namespace opal { namespace {

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

void pack_plane(const std::uint8_t *src,int stride,int width,int height,std::vector<std::uint8_t>&scratch){
    scratch.resize(static_cast<std::size_t>(width)*static_cast<std::size_t>(height));
    for(int y=0;y<height;++y)std::memcpy(scratch.data()+static_cast<std::size_t>(y)*width,src+static_cast<std::ptrdiff_t>(y)*stride,width);
}

const void *contiguous_plane(const std::uint8_t *src,int stride,int width,int height,std::vector<std::uint8_t>&scratch){
    if(stride==width)return src;
    pack_plane(src,stride,width,height,scratch);
    return scratch.data();
}

void request_fullscreen(Display *display,Window window){
    Atom state=XInternAtom(display,"_NET_WM_STATE",False);
    Atom fullscreen=XInternAtom(display,"_NET_WM_STATE_FULLSCREEN",False);
    if(state==None||fullscreen==None)return;
    XEvent event{};event.type=ClientMessage;event.xclient.window=window;event.xclient.message_type=state;event.xclient.format=32;
    event.xclient.data.l[0]=1;event.xclient.data.l[1]=static_cast<long>(fullscreen);
    XSendEvent(display,DefaultRootWindow(display),False,SubstructureRedirectMask|SubstructureNotifyMask,&event);
}

}}

struct VideoPresenter::Impl {
    Display *display=nullptr;
    Window window=0;
    Colormap colormap=0;
    GLXContext context=nullptr;
    GLuint program=0;
    std::array<GLuint,4> textures{};
    int source_width=0,source_height=0;

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
        glUseProgram(0);
        return true;
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
    impl_->window=XCreateWindow(impl_->display,RootWindow(impl_->display,screen),0,0,width,height,0,visual->depth,InputOutput,visual->visual,CWColormap|CWEventMask,&swa);
    if(impl_->window)impl_->context=glXCreateContext(impl_->display,visual,nullptr,True);
    XFree(visual);
    if(!impl_->window||!impl_->context){close();return false;}
    XStoreName(impl_->display,impl_->window,"OPAL");
    XMapRaised(impl_->display,impl_->window);XFlush(impl_->display);
    if(fullscreen)request_fullscreen(impl_->display,impl_->window);
    if(!glXMakeCurrent(impl_->display,impl_->window,impl_->context)){close();return false;}
    impl_->source_width=source_width;impl_->source_height=source_height;
    if(!impl_->init_gl()){close();return false;}
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    return true;
}

bool VideoPresenter::present(DecodedVideoFrame decoded){
    AVFrame *frame=decoded.frame;
    if(!frame)return false;
    auto release=[&]{av_frame_free(&frame);};
    if(!impl_||!impl_->display||!impl_->window||!impl_->context){release();return false;}
    if(frame->width<=0||frame->height<=0){release();return false;}
    bool yuv420=frame->format==AV_PIX_FMT_YUV420P||frame->format==AV_PIX_FMT_YUVJ420P;
    bool nv12=frame->format==AV_PIX_FMT_NV12;
    if(!yuv420&&!nv12){release();return false;}
    if(!glXMakeCurrent(impl_->display,impl_->window,impl_->context)){release();return false;}
    XWindowAttributes wa{};XGetWindowAttributes(impl_->display,impl_->window,&wa);glViewport(0,0,std::max(1,wa.width),std::max(1,wa.height));
    std::vector<std::uint8_t> scratch_y,scratch_u,scratch_v,scratch_uv;
    const void *y=contiguous_plane(frame->data[0],frame->linesize[0],frame->width,frame->height,scratch_y);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,impl_->textures[0]);glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,frame->width,frame->height,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,y);
    if(yuv420){
        int cw=(frame->width+1)/2,ch=(frame->height+1)/2;
        const void *u=contiguous_plane(frame->data[1],frame->linesize[1],cw,ch,scratch_u);
        const void *v=contiguous_plane(frame->data[2],frame->linesize[2],cw,ch,scratch_v);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,impl_->textures[1]);glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,cw,ch,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,u);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,impl_->textures[2]);glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,cw,ch,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,v);
    }else{
        int cw=(frame->width+1)/2,ch=(frame->height+1)/2,row_bytes=cw*2;
        const void *uv=contiguous_plane(frame->data[1],frame->linesize[1],row_bytes,ch,scratch_uv);
        glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,impl_->textures[3]);glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE_ALPHA,cw,ch,0,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,uv);
    }
    glUseProgram(impl_->program);glUniform1i(glGetUniformLocation(impl_->program,"nv12"),nv12?1:0);
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
