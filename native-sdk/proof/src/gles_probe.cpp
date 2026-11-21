#include "magpie/magpie.h"
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <cstring>
namespace { void init(MagpieBackendProbeV1* o){std::memset(o,0,sizeof(*o));o->struct_size=sizeof(*o);std::strncpy(o->reason,"probe only; FSR dispatch is not implemented in this proof library",sizeof(o->reason)-1);} }
extern "C" MAGPIE_API MagpieStatusV1 magpie_gles_probe_v1(MagpieBackendProbeV1* out){
 if(!out||out->struct_size<sizeof(*out))return MAGPIE_INVALID_ARGUMENT; init(out);
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLint ma=0,mi=0; if(d==EGL_NO_DISPLAY||!eglInitialize(d,&ma,&mi)){std::strncpy(out->reason,"EGL display/initialization unavailable",sizeof(out->reason)-1);return MAGPIE_UNAVAILABLE;}
 EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE}; EGLConfig cfg{}; EGLint count=0; if(!eglChooseConfig(d,attrs,&cfg,1,&count)||count<1){eglTerminate(d);std::strncpy(out->reason,"no GLES3 pbuffer config",sizeof(out->reason)-1);return MAGPIE_UNAVAILABLE;}
 EGLint pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE}; EGLSurface s=eglCreatePbufferSurface(d,cfg,pa); EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; eglBindAPI(EGL_OPENGL_ES_API); EGLContext c=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);
 if(s==EGL_NO_SURFACE||c==EGL_NO_CONTEXT||!eglMakeCurrent(d,s,s,c)){if(c!=EGL_NO_CONTEXT)eglDestroyContext(d,c);if(s!=EGL_NO_SURFACE)eglDestroySurface(d,s);eglTerminate(d);std::strncpy(out->reason,"GLES3 EGL context creation failed",sizeof(out->reason)-1);return MAGPIE_UNAVAILABLE;}
 GLint major=0,minor=0,workgroups=0; glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor); if(major>3||(major==3&&minor>=1)){glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS,&workgroups);out->runtime_available=1;out->compute_available=workgroups>=64;auto*v=glGetString(GL_RENDERER);if(v)std::strncpy(out->device_name,reinterpret_cast<const char*>(v),sizeof(out->device_name)-1);std::strncpy(out->reason,"GLES 3.1 compute context probed; no FSR dispatch performed",sizeof(out->reason)-1);}else std::strncpy(out->reason,"GLES 3.1 compute is unavailable",sizeof(out->reason)-1);
 eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,c);eglDestroySurface(d,s);eglTerminate(d);return out->runtime_available?MAGPIE_OK:MAGPIE_UNAVAILABLE;
}
