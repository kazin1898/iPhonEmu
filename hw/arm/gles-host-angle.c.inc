/* EGL lifetime adapter for the guest executor on ANGLE: Metal on macOS (the
 * experimental ltm_angle_prefix build), Direct3D 11 on Windows (MSYS2's
 * angleproject, docs/windows-gl.md). Each guest context has its own native
 * context, of the guest's API: ES 1.1 for an ES1 EAGL context, ES 2.0 (which
 * ANGLE hands out as 3.0) for an ES2 one. The group retains a root only for
 * the shared namespace; ANGLE shares between an ES1 and an ES2 context. Each
 * context draws into its own FBOs, so the EGL surface is a 1x1 pbuffer and
 * nothing is ever swapped. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#define CGLContextObj GLESAngleContextRef
#define CGLSetCurrentContext gles_angle_set_current
#define CGLRetainContext gles_angle_retain
#define CGLReleaseContext gles_angle_release
typedef struct GLESAngleContext {
    EGLContext context;
    EGLSurface surface;
    unsigned refs;
    int api;                    /* 1 or 2 */
} *CGLContextObj;
static EGLDisplay angle_display = EGL_NO_DISPLAY;
static EGLConfig angle_config;
static __thread CGLContextObj angle_current;
/* The current context speaks ES2: framebuffer objects by their core names. */
static __thread bool angle_es2;

#ifdef GLES_HOST_ANGLE_D3D11
#define GLES_ANGLE_PLATFORM EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE
#else
#define GLES_ANGLE_PLATFORM EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#endif

static bool gles_angle_create(CGLContextObj share, int api, CGLContextObj *out)
{
    if (angle_display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLint attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, GLES_ANGLE_PLATFORM, EGL_NONE};
        EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count;
        if (!get) return false;
        angle_display = get(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
        if (angle_display == EGL_NO_DISPLAY || !eglInitialize(angle_display, NULL, NULL) ||
            !eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(angle_display, ca, &angle_config, 1, &count) || !count) {
            fprintf(stderr, "[gles-angle] no EGL display/config (0x%x)\n", eglGetError());
            angle_display = EGL_NO_DISPLAY;
            return false;
        }
        fprintf(stderr, "[gles-angle] EGL %s\n", eglQueryString(angle_display, EGL_VERSION));
    }
    EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, api, EGL_NONE};
    EGLint surface[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    CGLContextObj c = g_new0(struct GLESAngleContext, 1);
    c->refs = 1;
    c->api = api;
    c->context = eglCreateContext(angle_display, angle_config, share ? share->context : EGL_NO_CONTEXT, ctx);
    c->surface = eglCreatePbufferSurface(angle_display, angle_config, surface);
    if (c->context == EGL_NO_CONTEXT || c->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[gles-angle] ES%d context failed (0x%x)\n", api, eglGetError());
        if (c->context != EGL_NO_CONTEXT) eglDestroyContext(angle_display, c->context);
        if (c->surface != EGL_NO_SURFACE) eglDestroySurface(angle_display, c->surface);
        g_free(c);
        return false;
    }
    *out = c;
    return true;
}
static int CGLSetCurrentContext(CGLContextObj c)
{
    if (angle_current == c) return 0;
    if (!eglMakeCurrent(angle_display, c ? c->surface : EGL_NO_SURFACE,
                        c ? c->surface : EGL_NO_SURFACE, c ? c->context : EGL_NO_CONTEXT)) return -1;
    angle_current = c;
    angle_es2 = c && c->api == 2;
    return 0;
}
static CGLContextObj CGLRetainContext(CGLContextObj c) { c->refs++; return c; }
static void CGLReleaseContext(CGLContextObj c)
{
    if (--c->refs) return;
    if (angle_current == c) CGLSetCurrentContext(NULL);
    eglDestroyContext(angle_display, c->context);
    eglDestroySurface(angle_display, c->surface);
    g_free(c);
}

/* Desktop clip-plane setters receive doubles reconstructed from guest floats. */
static void gles_angle_clip_plane(GLenum plane, const double *value)
{
    GLfloat f[4] = {value[0], value[1], value[2], value[3]};
    glClipPlanef(plane, f);
}
#define glClipPlane gles_angle_clip_plane

/* Framebuffer objects: ES 1.1 has them only as OES_framebuffer_object, ES 2.0
 * only by the core names, and ANGLE rejects the other spelling in each
 * (GL_INVALID_OPERATION). Same tokens and arguments either way. */
#define GLES_ANGLE_FBO(ret, name, params, args) \
    static ret gles_angle_##name params \
    { \
        if (angle_es2) return gl##name args; \
        return gl##name##OES args; \
    }
GLES_ANGLE_FBO(GLboolean, IsFramebuffer, (GLuint n), (n))
GLES_ANGLE_FBO(GLboolean, IsRenderbuffer, (GLuint n), (n))
GLES_ANGLE_FBO(void, GenerateMipmap, (GLenum t), (t))
GLES_ANGLE_FBO(void, GenFramebuffers, (GLsizei n, GLuint *p), (n, p))
GLES_ANGLE_FBO(void, BindFramebuffer, (GLenum t, GLuint n), (t, n))
GLES_ANGLE_FBO(void, FramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint n, GLint l), (t, a, tt, n, l))
GLES_ANGLE_FBO(void, GenRenderbuffers, (GLsizei n, GLuint *p), (n, p))
GLES_ANGLE_FBO(void, BindRenderbuffer, (GLenum t, GLuint n), (t, n))
GLES_ANGLE_FBO(void, RenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h), (t, f, w, h))
GLES_ANGLE_FBO(void, FramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint n), (t, a, rt, n))
GLES_ANGLE_FBO(GLenum, CheckFramebufferStatus, (GLenum t), (t))
GLES_ANGLE_FBO(void, DeleteFramebuffers, (GLsizei n, const GLuint *p), (n, p))
GLES_ANGLE_FBO(void, DeleteRenderbuffers, (GLsizei n, const GLuint *p), (n, p))
GLES_ANGLE_FBO(void, GetRenderbufferParameteriv, (GLenum t, GLenum p, GLint *v), (t, p, v))
GLES_ANGLE_FBO(void, GetFramebufferAttachmentParameteriv, (GLenum t, GLenum a, GLenum p, GLint *v), (t, a, p, v))
#undef GLES_ANGLE_FBO
#define glIsFramebufferEXT              gles_angle_IsFramebuffer
#define glIsRenderbufferEXT             gles_angle_IsRenderbuffer
#define glGenerateMipmapEXT             gles_angle_GenerateMipmap
#define glGenFramebuffersEXT            gles_angle_GenFramebuffers
#define glBindFramebufferEXT            gles_angle_BindFramebuffer
#define glFramebufferTexture2DEXT       gles_angle_FramebufferTexture2D
#define glGenRenderbuffersEXT           gles_angle_GenRenderbuffers
#define glBindRenderbufferEXT           gles_angle_BindRenderbuffer
#define glRenderbufferStorageEXT        gles_angle_RenderbufferStorage
#define glFramebufferRenderbufferEXT    gles_angle_FramebufferRenderbuffer
#define glCheckFramebufferStatusEXT     gles_angle_CheckFramebufferStatus
#define glDeleteFramebuffersEXT         gles_angle_DeleteFramebuffers
#define glDeleteRenderbuffersEXT        gles_angle_DeleteRenderbuffers
#define glGetRenderbufferParameterivEXT gles_angle_GetRenderbufferParameteriv
#define glGetFramebufferAttachmentParameterivEXT gles_angle_GetFramebufferAttachmentParameteriv
