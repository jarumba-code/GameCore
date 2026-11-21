// Runtime EGL/GLES capability probe for Android system libraries.
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <cstdio>

int main() {
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
    std::fprintf(stderr, "EGL initialization failed: 0x%x\n", eglGetError());
    return 2;
  }
  const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig config{};
  EGLint configCount = 0;
  if (!eglChooseConfig(display, configAttrs, &config, 1, &configCount) ||
      configCount == 0) {
    std::fprintf(stderr, "No pbuffer ES3 EGL config: 0x%x\n", eglGetError());
    return 3;
  }
  const EGLint surfaceAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                        contextAttrs);
  if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(display, surface, surface, context)) {
    std::fprintf(stderr, "ES3 context creation failed: 0x%x\n", eglGetError());
    return 4;
  }
  GLint glMajor = 0, glMinor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
  glGetIntegerv(GL_MINOR_VERSION, &glMinor);
  std::printf("egl=%d.%d egl_vendor=%s gl=%s glsl=%s renderer=%s version=%d.%d\n",
              major, minor, eglQueryString(display, EGL_VENDOR),
              glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION),
              glGetString(GL_RENDERER), glMajor, glMinor);
  std::printf("compute_core=%s image_load_store_core=%s\n",
              (glMajor > 3 || (glMajor == 3 && glMinor >= 1)) ? "yes" : "no",
              (glMajor > 3 || (glMajor == 3 && glMinor >= 1)) ? "yes" : "no");
  eglDestroyContext(display, context);
  eglDestroySurface(display, surface);
  eglTerminate(display);
  return (glMajor > 3 || (glMajor == 3 && glMinor >= 1)) ? 0 : 5;
}
