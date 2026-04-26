// waywallen-mpv-renderer — libmpv + OpenGL ES/EGL video renderer subprocess
// for the waywallen daemon. Spawned for wallpapers of type "video".

#include <waywallen-bridge/bridge.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <gbm.h>
#include <fcntl.h>
#include <errno.h>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {

constexpr uint32_t SLOT_COUNT          = 3;
// DRM fourcc constants. ABGR8888 (memory order R,G,B,A) matches GL_RGBA8
// channel layout on Mesa, but some drivers only accept other fourccs as
// non-external GL_TEXTURE_2D bind targets — so we probe at runtime
// across the full 8888 family (cf. libfunnel's egl.c try_format()).
constexpr uint32_t DRM_FORMAT_ABGR8888 = 0x34324241u; // 'A','B','2','4'
constexpr uint32_t DRM_FORMAT_XBGR8888 = 0x34324258u; // 'X','B','2','4'
constexpr uint32_t DRM_FORMAT_ARGB8888 = 0x34325241u; // 'A','R','2','4'
constexpr uint32_t DRM_FORMAT_XRGB8888 = 0x34325258u; // 'X','R','2','4'
constexpr uint32_t DRM_FORMAT_RGBA8888 = 0x41424752u; // 'R','G','B','A'
constexpr uint32_t DRM_FORMAT_BGRA8888 = 0x41524742u; // 'B','G','R','A'
constexpr uint32_t DRM_FORMAT_RGBX8888 = 0x58424752u; // 'R','G','B','X'
constexpr uint32_t DRM_FORMAT_BGRX8888 = 0x58524742u; // 'B','G','R','X'

struct Options {
    std::string ipc_path;
    std::string video_path;
    uint32_t    width { 1280 };
    uint32_t    height { 720 };
    bool        loop_file { true };
    bool        hwdec { true };
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-mpv-renderer: %s\n", msg.c_str());
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (a == "--ipc") {
            o.ipc_path = next();
        } else if (a == "--width") {
            o.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--height") {
            o.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--video" || a == "--path") {
            o.video_path = next();
        } else if (a == "--no-hwdec") {
            o.hwdec = false;
        } else if (a == "--no-loop") {
            o.loop_file = false;
        } else {
            // Swallow unknown --key value pairs forwarded from daemon
            // source-plugin metadata. Notably drops --fps: pacing is now
            // fully driven by libmpv's render-update callback.
            if (!a.empty() && a.rfind("--", 0) == 0 && i + 1 < argc
                && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                ++i;
            }
        }
    }
    return o;
}

uint64_t now_ns() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}


// ---------------------------------------------------------------------------
// EGL / GLES
// ---------------------------------------------------------------------------

struct GlFns {
    PFNEGLGETPLATFORMDISPLAYEXTPROC          eglGetPlatformDisplayEXT { nullptr };
    PFNEGLCREATEIMAGEKHRPROC                 eglCreateImageKHR { nullptr };
    PFNEGLDESTROYIMAGEKHRPROC                eglDestroyImageKHR { nullptr };
    PFNEGLCREATESYNCKHRPROC                  eglCreateSyncKHR { nullptr };
    PFNEGLDESTROYSYNCKHRPROC                 eglDestroySyncKHR { nullptr };
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC        eglDupNativeFenceFDANDROID { nullptr };
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC        eglQueryDmaBufModifiersEXT { nullptr };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC      glEGLImageTargetTexture2DOES { nullptr };
};

struct Slot {
    // Native GL_RGBA8 intermediate that mpv renders into. Decouples mpv's
    // pipeline from any driver restriction on the DMA-BUF EGLImage (e.g.
    // external_only modifiers that reject GL_TEXTURE_2D bind).
    GLuint         mpv_texture { 0 };
    GLuint         mpv_fbo { 0 };
    // DMA-BUF-backed export target. Filled via glBlitFramebuffer from
    // mpv_fbo each frame.
    GLuint         export_texture { 0 };
    GLuint         export_fbo { 0 };
    EGLImageKHR    egl_image { EGL_NO_IMAGE_KHR };
    struct gbm_bo* bo { nullptr };
    int            dmabuf_fd { -1 };
    uint64_t       drm_modifier { 0 };
    uint32_t       size { 0 };
    uint32_t       stride { 0 };
    uint32_t       offset { 0 };
};

struct GlCtx {
    int                drm_fd { -1 };
    struct gbm_device* gbm { nullptr };
    EGLDisplay         display { EGL_NO_DISPLAY };
    EGLContext         context { EGL_NO_CONTEXT };
    GlFns              fns;
    Slot               slots[SLOT_COUNT];
    // Chosen via eglQueryDmaBufModifiersEXT at init (or legacy probe).
    uint32_t           export_fourcc { 0 };
    uint64_t           export_modifier { 0 };
    // false ⇒ legacy NVIDIA-style path: gbm_bo_create(USE_LINEAR) and
    // import without EGL modifier attrs. Used when EGL's importable
    // modifier set has no overlap with GBM's producible set.
    bool               use_explicit_modifier { true };
};

void* must_egl_proc(const char* name) {
    void* p = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (!p) die(std::string("eglGetProcAddress missing: ") + name);
    return p;
}

bool egl_has_ext(const char* exts, const char* e) {
    return exts && std::strstr(exts, e) != nullptr;
}

// Open the DRM render node bound to the EGL display, falling back to
// a known-paths scan. Pattern adopted from libfunnel/src/egl.c so EGL
// and GBM end up on the same physical GPU on multi-GPU systems.
// Must be called after eglInitialize.
void open_render_node(GlCtx& gl) {
    const char* node = nullptr;
    auto eglQueryDisplayAttribEXT_ =
        reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
            eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto eglQueryDeviceStringEXT_ =
        reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
            eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (eglQueryDisplayAttribEXT_ && eglQueryDeviceStringEXT_) {
        EGLAttrib dev_attr = 0;
        if (eglQueryDisplayAttribEXT_(gl.display, EGL_DEVICE_EXT, &dev_attr)
            && dev_attr) {
            EGLDeviceEXT dev = reinterpret_cast<EGLDeviceEXT>(dev_attr);
            node = eglQueryDeviceStringEXT_(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (!node)
                node = eglQueryDeviceStringEXT_(dev, EGL_DRM_DEVICE_FILE_EXT);
        }
    }
    if (node) {
        gl.drm_fd = ::open(node, O_RDWR | O_CLOEXEC);
        if (gl.drm_fd < 0) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: EGL reported %s but open() failed: %s; scanning\n",
                node, std::strerror(errno));
        } else {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: opened DRM render node %s via EGL_DEVICE_EXT (fd=%d)\n",
                node, gl.drm_fd);
        }
    }
    if (gl.drm_fd < 0) {
        const char* scan[] = {
            "/dev/dri/renderD128",
            "/dev/dri/renderD129",
        };
        for (const char* n : scan) {
            int fd = ::open(n, O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                gl.drm_fd = fd;
                std::fprintf(stderr,
                    "waywallen-mpv-renderer: opened DRM render node %s by scan (fd=%d)\n",
                    n, fd);
                break;
            }
        }
    }
    if (gl.drm_fd < 0) die("no DRM render node could be opened");
    gl.gbm = gbm_create_device(gl.drm_fd);
    if (!gl.gbm) die("gbm_create_device failed");
}

void pick_export_format(GlCtx& gl, const Options& opt);
void init_slots(GlCtx& gl, const Options& opt);
bool try_legacy_linear(GlCtx& gl, const Options& opt, uint32_t fourcc);

void init_egl(GlCtx& gl, const Options& opt) {
    gl.fns.eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            must_egl_proc("eglGetPlatformDisplayEXT"));

    // Headless rendering — no window, no output device needed. Mesa's
    // surfaceless platform is what lets us run a pure DMA-BUF producer.
    gl.display = gl.fns.eglGetPlatformDisplayEXT(
        EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (gl.display == EGL_NO_DISPLAY) {
        die("eglGetPlatformDisplay(SURFACELESS_MESA) failed; Mesa required");
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(gl.display, &major, &minor)) {
        die("eglInitialize failed");
    }

    const char* exts = eglQueryString(gl.display, EGL_EXTENSIONS);
    if (!egl_has_ext(exts, "EGL_KHR_surfaceless_context"))
        die("EGL_KHR_surfaceless_context missing");
    if (!egl_has_ext(exts, "EGL_KHR_image_base"))
        die("EGL_KHR_image_base missing");
    if (!egl_has_ext(exts, "EGL_EXT_image_dma_buf_import")
        || !egl_has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers"))
        die("EGL DMA-BUF import (modifiers) extensions missing");
    if (!egl_has_ext(exts, "EGL_KHR_fence_sync")
        || !egl_has_ext(exts, "EGL_ANDROID_native_fence_sync"))
        die("EGL fence-sync extensions missing");

    // Now that the display is initialized, open the DRM render node it's
    // bound to (libfunnel-style) and create the matching GBM device.
    open_render_node(gl);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) die("eglBindAPI(GLES) failed");

    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint    n_configs = 0;
    if (!eglChooseConfig(gl.display, config_attrs, &config, 1, &n_configs)
        || n_configs < 1) {
        die("eglChooseConfig: no GLES3 pbuffer config");
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
    gl.context = eglCreateContext(gl.display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (gl.context == EGL_NO_CONTEXT) die("eglCreateContext failed");

    if (!eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl.context))
        die("eglMakeCurrent(surfaceless) failed");

    auto str_or = [](const char* s) { return s ? s : "(null)"; };
    auto gl_str = [&](GLenum name) {
        return str_or(reinterpret_cast<const char*>(glGetString(name)));
    };
    std::fprintf(stderr,
        "waywallen-mpv-renderer: GPU info\n"
        "  egl vendor:  %s\n"
        "  egl version: %s (%d.%d)\n"
        "  egl client:  %s\n"
        "  gl vendor:   %s\n"
        "  gl renderer: %s\n"
        "  gl version:  %s\n"
        "  glsl ver:    %s\n",
        str_or(eglQueryString(gl.display, EGL_VENDOR)),
        str_or(eglQueryString(gl.display, EGL_VERSION)), major, minor,
        str_or(eglQueryString(gl.display, EGL_CLIENT_APIS)),
        gl_str(GL_VENDOR),
        gl_str(GL_RENDERER),
        gl_str(GL_VERSION),
        gl_str(GL_SHADING_LANGUAGE_VERSION));

    gl.fns.eglCreateImageKHR =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
            must_egl_proc("eglCreateImageKHR"));
    gl.fns.eglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            must_egl_proc("eglDestroyImageKHR"));
    gl.fns.eglCreateSyncKHR =
        reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
            must_egl_proc("eglCreateSyncKHR"));
    gl.fns.eglDestroySyncKHR =
        reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
            must_egl_proc("eglDestroySyncKHR"));
    gl.fns.eglDupNativeFenceFDANDROID =
        reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            must_egl_proc("eglDupNativeFenceFDANDROID"));
    gl.fns.eglQueryDmaBufModifiersEXT =
        reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
            must_egl_proc("eglQueryDmaBufModifiersEXT"));
    // GL_OES_EGL_image: required to wrap an EGLImage as a GL_TEXTURE_2D.
    const GLubyte* gl_exts = glGetString(GL_EXTENSIONS);
    if (!gl_exts
        || !std::strstr(reinterpret_cast<const char*>(gl_exts),
                        "GL_OES_EGL_image")) {
        die("GL_OES_EGL_image missing");
    }
    gl.fns.glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            must_egl_proc("glEGLImageTargetTexture2DOES"));

    pick_export_format(gl, opt);
    init_slots(gl, opt);
}

const char* fourcc_name(uint32_t f) {
    switch (f) {
        case DRM_FORMAT_ABGR8888: return "ABGR8888";
        case DRM_FORMAT_XBGR8888: return "XBGR8888";
        case DRM_FORMAT_ARGB8888: return "ARGB8888";
        case DRM_FORMAT_XRGB8888: return "XRGB8888";
        case DRM_FORMAT_RGBA8888: return "RGBA8888";
        case DRM_FORMAT_BGRA8888: return "BGRA8888";
        case DRM_FORMAT_RGBX8888: return "RGBX8888";
        case DRM_FORMAT_BGRX8888: return "BGRX8888";
        default:                  return "?";
    }
}

// Pick a (fourcc, modifier) pair that satisfies BOTH:
//   1) EGL can import it as a non-external GL_TEXTURE_2D
//      (i.e. eglQueryDmaBufModifiersEXT external_only == EGL_FALSE), AND
//   2) GBM can produce it with GBM_BO_USE_RENDERING.
//
// (1) and (2) are not the same set — EGL's importer accepts vendor tilings
// the GBM allocator's producer path won't actually emit. So we hand the
// full filtered set to gbm_bo_create_with_modifiers2 and let the allocator
// pick a modifier from the intersection. The actual modifier chosen is
// pinned and reused for every slot to keep the BindBuffers metadata stable.
void pick_export_format(GlCtx& gl, const Options& opt) {
    // Mirrors libfunnel/src/egl.c: alpha + alpha-less, both byte orders.
    const uint32_t cands[] = {
        DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
        DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888,
        DRM_FORMAT_RGBX8888, DRM_FORMAT_BGRX8888,
    };
    for (uint32_t fourcc : cands) {
        EGLint count = 0;
        if (!gl.fns.eglQueryDmaBufModifiersEXT(
                gl.display, static_cast<EGLint>(fourcc),
                0, nullptr, nullptr, &count) || count <= 0) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: %s: no modifiers reported by EGL\n",
                fourcc_name(fourcc));
            continue;
        }
        std::vector<EGLuint64KHR> mods(static_cast<size_t>(count));
        std::vector<EGLBoolean>   ext_only(static_cast<size_t>(count));
        if (!gl.fns.eglQueryDmaBufModifiersEXT(
                gl.display, static_cast<EGLint>(fourcc), count,
                mods.data(), ext_only.data(), &count)) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: %s: eglQueryDmaBufModifiersEXT failed\n",
                fourcc_name(fourcc));
            continue;
        }
        std::fprintf(stderr,
            "waywallen-mpv-renderer: %s: %d modifiers reported by EGL\n",
            fourcc_name(fourcc), count);
        for (int i = 0; i < count; ++i) {
            std::fprintf(stderr, "  - 0x%016llx [external_only=%d]\n",
                static_cast<unsigned long long>(mods[i]),
                static_cast<int>(ext_only[i]));
        }
        std::vector<uint64_t> usable;
        usable.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (!ext_only[i]) usable.push_back(static_cast<uint64_t>(mods[i]));
        }
        if (usable.empty()) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: %s: all %d modifiers are external_only\n",
                fourcc_name(fourcc), count);
            continue;
        }
        // Float LINEAR to the front so GBM prefers it when allocator-feasible.
        for (size_t i = 0; i < usable.size(); ++i) {
            if (usable[i] == DRM_FORMAT_MOD_LINEAR) {
                std::swap(usable[0], usable[i]);
                break;
            }
        }
        // Probe-allocate one BO. GBM internally picks a modifier from the
        // offered list that its producer side can actually produce. If it
        // returns nullptr for the whole list, GBM and EGL disagree on this
        // fourcc — try the next.
        gbm_bo* probe = gbm_bo_create_with_modifiers2(
            gl.gbm, opt.width, opt.height, fourcc,
            usable.data(), usable.size(), GBM_BO_USE_RENDERING);
        if (!probe) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: %s: gbm rejected all %zu non-external modifiers\n",
                fourcc_name(fourcc), usable.size());
            continue;
        }
        gl.export_fourcc           = fourcc;
        gl.export_modifier         = gbm_bo_get_modifier(probe);
        gl.use_explicit_modifier   = true;
        std::fprintf(stderr,
            "waywallen-mpv-renderer: chose export fourcc=%s modifier=0x%016llx "
            "(probed from %zu non-external candidates)\n",
            fourcc_name(fourcc),
            static_cast<unsigned long long>(gl.export_modifier), usable.size());
        gbm_bo_destroy(probe);
        return;
    }

    // NVIDIA-style fallback. EGL listed many tiled modifiers it accepts
    // (vendor=0x03), but NVIDIA's GBM only allocates LINEAR, and LINEAR
    // is external_only on the modifier-tagged path — so the intersection
    // above is empty. Legacy import (no modifier attrs) on top of an
    // implicit-LINEAR BO is NVIDIA's standard EGL/GBM interop recipe and
    // typically yields a TEXTURE_2D-bindable image.
    std::fprintf(stderr,
        "waywallen-mpv-renderer: modifier-aware probe failed, "
        "falling back to legacy implicit-modifier LINEAR import\n");
    const uint32_t legacy_cands[] = {
        DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
        DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888,
        DRM_FORMAT_RGBX8888, DRM_FORMAT_BGRX8888,
    };
    for (uint32_t fourcc : legacy_cands) {
        if (try_legacy_linear(gl, opt, fourcc)) {
            gl.export_fourcc         = fourcc;
            gl.use_explicit_modifier = false;
            std::fprintf(stderr,
                "waywallen-mpv-renderer: chose export fourcc=%s "
                "via legacy LINEAR (modifier=0x%016llx, no modifier attrs on import)\n",
                fourcc_name(fourcc),
                static_cast<unsigned long long>(gl.export_modifier));
            return;
        }
    }
    die("no DMA-BUF (fourcc, modifier) supports both EGL import and GBM render-allocation");
}

// Legacy probe: allocate one LINEAR BO without modifier negotiation,
// import via EGL_LINUX_DMA_BUF_EXT with no EGL_DMA_BUF_PLANE0_MODIFIER_*
// attributes, and verify the result can be bound as GL_TEXTURE_2D and
// used as a color attachment. On success, leaves gl.export_modifier set
// to whatever GBM reports for the BO (LINEAR or INVALID, depending on
// driver) so send_bind passes a faithful value to the daemon.
bool try_legacy_linear(GlCtx& gl, const Options& opt, uint32_t fourcc) {
    gbm_bo* bo = gbm_bo_create(
        gl.gbm, opt.width, opt.height, fourcc,
        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) {
        std::fprintf(stderr,
            "waywallen-mpv-renderer: %s: legacy gbm_bo_create(USE_LINEAR) failed\n",
            fourcc_name(fourcc));
        return false;
    }
    int      dmabuf_fd = gbm_bo_get_fd(bo);
    uint32_t stride    = gbm_bo_get_stride(bo);
    uint32_t offset    = gbm_bo_get_offset(bo, 0);
    uint64_t modifier  = gbm_bo_get_modifier(bo);
    if (dmabuf_fd < 0) {
        gbm_bo_destroy(bo);
        return false;
    }

    EGLint attrs[] = {
        EGL_WIDTH,                     static_cast<EGLint>(opt.width),
        EGL_HEIGHT,                    static_cast<EGLint>(opt.height),
        EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT,     dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(stride),
        EGL_NONE,
    };
    EGLImageKHR img = gl.fns.eglCreateImageKHR(
        gl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    bool ok = false;
    if (img == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr,
            "waywallen-mpv-renderer: %s: legacy eglCreateImageKHR failed (egl_err=0x%04x)\n",
            fourcc_name(fourcc), eglGetError());
    } else {
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        (void)glGetError();
        gl.fns.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        GLenum bind_err = glGetError();
        if (bind_err != GL_NO_ERROR) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: %s: legacy bind to TEXTURE_2D rejected "
                "(gl_err=0x%04x — likely external_only)\n",
                fourcc_name(fourcc), bind_err);
        } else {
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex, 0);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st == GL_FRAMEBUFFER_COMPLETE) {
                ok = true;
                gl.export_modifier = modifier;
            } else {
                std::fprintf(stderr,
                    "waywallen-mpv-renderer: %s: legacy FBO incomplete (status=0x%04x)\n",
                    fourcc_name(fourcc), st);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        if (fbo) glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        gl.fns.eglDestroyImageKHR(gl.display, img);
    }
    close(dmabuf_fd);
    gbm_bo_destroy(bo);
    return ok;
}

void init_slots(GlCtx& gl, const Options& opt) {
    const uint64_t mods[1] = { gl.export_modifier };
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        Slot& s = gl.slots[i];

        // 1) mpv intermediate target: ordinary GL_RGBA8 texture + FBO.
        //    No DMA-BUF involvement here, so this can never fail for
        //    driver-specific external_only / modifier reasons.
        glGenTextures(1, &s.mpv_texture);
        glBindTexture(GL_TEXTURE_2D, s.mpv_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     static_cast<GLsizei>(opt.width),
                     static_cast<GLsizei>(opt.height),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &s.mpv_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.mpv_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s.mpv_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            die("mpv intermediate FBO incomplete");

        // 2) Export DMA-BUF, allocated and imported per the strategy
        //    pick_export_format settled on.
        if (gl.use_explicit_modifier) {
            s.bo = gbm_bo_create_with_modifiers2(
                gl.gbm, opt.width, opt.height, gl.export_fourcc,
                mods, 1, GBM_BO_USE_RENDERING);
            if (!s.bo) die("gbm_bo_create_with_modifiers2 failed");
        } else {
            s.bo = gbm_bo_create(
                gl.gbm, opt.width, opt.height, gl.export_fourcc,
                GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
            if (!s.bo) die("gbm_bo_create(USE_LINEAR) failed");
        }

        s.dmabuf_fd    = gbm_bo_get_fd(s.bo);
        if (s.dmabuf_fd < 0) die("gbm_bo_get_fd failed");
        s.stride       = gbm_bo_get_stride(s.bo);
        s.offset       = gbm_bo_get_offset(s.bo, 0);
        s.drm_modifier = gbm_bo_get_modifier(s.bo);
        s.size         = s.stride * opt.height;

        EGLint attrs_modifier[] = {
            EGL_WIDTH,                          static_cast<EGLint>(opt.width),
            EGL_HEIGHT,                         static_cast<EGLint>(opt.height),
            EGL_LINUX_DRM_FOURCC_EXT,           static_cast<EGLint>(gl.export_fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT,          s.dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,      static_cast<EGLint>(s.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,       static_cast<EGLint>(s.stride),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                static_cast<EGLint>(s.drm_modifier & 0xffffffffULL),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                static_cast<EGLint>((s.drm_modifier >> 32) & 0xffffffffULL),
            EGL_NONE,
        };
        EGLint attrs_legacy[] = {
            EGL_WIDTH,                     static_cast<EGLint>(opt.width),
            EGL_HEIGHT,                    static_cast<EGLint>(opt.height),
            EGL_LINUX_DRM_FOURCC_EXT,      static_cast<EGLint>(gl.export_fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT,     s.dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(s.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,  static_cast<EGLint>(s.stride),
            EGL_NONE,
        };
        s.egl_image = gl.fns.eglCreateImageKHR(
            gl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
            gl.use_explicit_modifier ? attrs_modifier : attrs_legacy);
        if (s.egl_image == EGL_NO_IMAGE_KHR)
            die("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) failed");

        glGenTextures(1, &s.export_texture);
        glBindTexture(GL_TEXTURE_2D, s.export_texture);
        (void)glGetError(); // clear pending errors
        gl.fns.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, s.egl_image);
        GLenum gl_err_after_image = glGetError();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &s.export_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s.export_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s.export_texture, 0);
        GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr,
                "waywallen-mpv-renderer: slot[%u] export FBO incomplete: "
                "status=0x%04x gl_err_after_image=0x%04x gl_err_now=0x%04x "
                "size=%ux%u stride=%u offset=%u modifier=0x%016llx fourcc=%s fd=%d\n",
                i, fbo_status, gl_err_after_image, glGetError(),
                opt.width, opt.height, s.stride, s.offset,
                static_cast<unsigned long long>(s.drm_modifier),
                fourcc_name(gl.export_fourcc), s.dmabuf_fd);
            die("export FBO incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

int export_sync_fd(GlCtx& gl) {
    EGLSyncKHR sync = gl.fns.eglCreateSyncKHR(
        gl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    // Ensure the sync is inserted into the command stream *before* we
    // dup the fd, otherwise the native fence may be empty.
    glFlush();
    int fd = gl.fns.eglDupNativeFenceFDANDROID(gl.display, sync);
    gl.fns.eglDestroySyncKHR(gl.display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) return -1;
    return fd;
}

void destroy_gl(GlCtx& gl) {
    if (gl.display != EGL_NO_DISPLAY) {
        for (auto& s : gl.slots) {
            if (s.egl_image != EGL_NO_IMAGE_KHR)
                gl.fns.eglDestroyImageKHR(gl.display, s.egl_image);
            if (s.export_fbo)     glDeleteFramebuffers(1, &s.export_fbo);
            if (s.export_texture) glDeleteTextures(1, &s.export_texture);
            if (s.mpv_fbo)        glDeleteFramebuffers(1, &s.mpv_fbo);
            if (s.mpv_texture)    glDeleteTextures(1, &s.mpv_texture);
            if (s.dmabuf_fd >= 0) close(s.dmabuf_fd);
            if (s.bo) gbm_bo_destroy(s.bo);
        }
        eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gl.context != EGL_NO_CONTEXT)
            eglDestroyContext(gl.display, gl.context);
        eglTerminate(gl.display);
    }
    if (gl.gbm) gbm_device_destroy(gl.gbm);
    if (gl.drm_fd >= 0) close(gl.drm_fd);
}


// ---------------------------------------------------------------------------
// mpv
// ---------------------------------------------------------------------------

struct MpvState {
    mpv_handle*         mpv { nullptr };
    mpv_render_context* ctx { nullptr };
};

struct WakeState {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    pending { false };
};

void on_mpv_render_update(void* ctx) {
    auto* w = static_cast<WakeState*>(ctx);
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->pending = true;
    }
    w->cv.notify_one();
}

void* mpv_get_proc_address(void* /*ctx*/, const char* name) {
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void mpv_init(MpvState& m, const Options& opt, WakeState& wake) {
    m.mpv = mpv_create();
    if (!m.mpv) die("mpv_create failed");

    mpv_set_option_string(m.mpv, "vo",                     "libmpv");
    mpv_set_option_string(m.mpv, "audio",                  "no");
    mpv_set_option_string(m.mpv, "terminal",               "no");
    mpv_set_option_string(m.mpv, "msg-level",              "all=warn");
    mpv_set_option_string(m.mpv, "loop-file",
                          opt.loop_file ? "inf" : "no");
    // With the GL render API, hwdec can stay on GPU (vaapi-egl etc.),
    // so "auto-safe" is the right default; SW render path used to stall.
    mpv_set_option_string(m.mpv, "hwdec",
                          opt.hwdec ? "auto-safe" : "no");
    mpv_set_option_string(m.mpv, "keep-open",              "always");
    mpv_set_option_string(m.mpv, "input-default-bindings", "no");
    mpv_set_option_string(m.mpv, "input-vo-keyboard",      "no");

    if (int rc = mpv_initialize(m.mpv); rc < 0)
        die(std::string("mpv_initialize: ") + mpv_error_string(rc));

    mpv_opengl_init_params gl_params {};
    gl_params.get_proc_address     = mpv_get_proc_address;
    gl_params.get_proc_address_ctx = nullptr;

    mpv_render_param create_params[] = {
        { MPV_RENDER_PARAM_API_TYPE,
          const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_params },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (int rc = mpv_render_context_create(&m.ctx, m.mpv, create_params); rc < 0)
        die(std::string("mpv_render_context_create: ")
            + mpv_error_string(rc));

    mpv_render_context_set_update_callback(m.ctx, on_mpv_render_update, &wake);

    if (!opt.video_path.empty()) {
        const char* cmd[] = { "loadfile", opt.video_path.c_str(), nullptr };
        if (int rc = mpv_command(m.mpv, cmd); rc < 0) {
            std::fprintf(stderr,
                         "waywallen-mpv-renderer: loadfile %s failed: %s\n",
                         opt.video_path.c_str(), mpv_error_string(rc));
        }
    }
}

bool mpv_render_into_slot(MpvState& m, GlCtx& gl, uint32_t slot,
                          const Options& opt) {
    // Pass 1: mpv renders into a native GL_RGBA8 intermediate FBO.
    mpv_opengl_fbo fbo_info {};
    fbo_info.fbo             = static_cast<int>(gl.slots[slot].mpv_fbo);
    fbo_info.w               = static_cast<int>(opt.width);
    fbo_info.h               = static_cast<int>(opt.height);
    fbo_info.internal_format = 0;

    // FLIP_Y=0 is GL convention (y=0 at bottom). The blit below copies
    // src y -> dst y unchanged, so the DMA-BUF FBO ends up byte-equivalent
    // to the previous direct-render path which also used FLIP_Y=0.
    int flip_y = 0;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo_info },
        { MPV_RENDER_PARAM_FLIP_Y,     &flip_y },
        { MPV_RENDER_PARAM_INVALID,    nullptr },
    };
    if (mpv_render_context_render(m.ctx, params) < 0) return false;

    // Pass 2: blit the intermediate into the DMA-BUF-backed export FBO.
    // Same dimensions and channel-compatible formats ⇒ a single GPU copy.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.slots[slot].mpv_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.slots[slot].export_fbo);
    glBlitFramebuffer(
        0, 0, static_cast<GLint>(opt.width), static_cast<GLint>(opt.height),
        0, 0, static_cast<GLint>(opt.width), static_cast<GLint>(opt.height),
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void mpv_drain_events(MpvState& m, std::atomic<bool>& shutdown) {
    while (true) {
        mpv_event* ev = mpv_wait_event(m.mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            shutdown.store(true, std::memory_order_release);
    }
}


// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------

struct HostState {
    int                   sock { -1 };
    std::mutex            send_mu; // serializes sendmsg on `sock`
    std::atomic<bool>     shutdown { false };
    std::atomic<uint64_t> seq { 0 };
    // bind_buffers generation: monotonic, bumped on every fresh
    // BindBuffers we emit. Initial bind = 1; subsequent re-emits
    // (after ConfigureBuffers) increment. Held under send_mu.
    uint64_t              bind_generation { 0 };
    // Mirrors the Slot pool's actual placement. GBM-allocated LINEAR
    // BOs land in GTT (host-visible from the kernel's POV) on every
    // driver we care about, so the pool is *always* PRIME-importable
    // by a foreign GPU. We therefore advertise BUF_HOST_VISIBLE
    // unconditionally: the GBM path simply doesn't expose a knob to
    // force DEVICE_LOCAL/VRAM placement, so a daemon ConfigureBuffers
    // request for flags=0 is best-effort acknowledged by re-emitting
    // bind_buffers carrying the *actual* flags=BUF_HOST_VISIBLE.
    uint32_t              flags { 1u /* BUF_HOST_VISIBLE */ };
    // Monotonic counter for `frame_ready.release_point`. Each new
    // submit publishes `++release_point` on the producer's release
    // timeline; the daemon's reaper TRANSFERs consumer release fences
    // onto that point. Producer back-pressure: before re-rendering
    // into slot `S`, we DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT on
    // `last_release_point[S]` so we don't overwrite a buffer the
    // compositor is still reading.
    uint64_t              release_point { 0 };
    // drm_syncobj handle owning the release timeline; exported once
    // via HANDLE_TO_FD and shipped to the daemon as a `ReleaseSyncobj`
    // event right after `Ready`.
    uint32_t              release_syncobj_handle { 0 };
    // Per-slot release point assigned at last submit. 0 means "never
    // submitted yet — no wait needed". Updated under `send_mu`.
    uint64_t              last_release_point[SLOT_COUNT] { 0, 0, 0 };
};

// Bit set passed on the wire as `bind_buffers.flags` and on
// `configure_buffers.flags`. Local mirror of the daemon-side constant
// so we don't pull in extra headers.
static constexpr uint32_t WW_BUF_HOST_VISIBLE = 1u << 0;

void wake_up(WakeState& w) {
    {
        std::lock_guard<std::mutex> lk(w.mu);
        w.pending = true;
    }
    w.cv.notify_one();
}

void send_bind(HostState& s, const Options& opt, GlCtx& gl) {
    uint64_t sizes[SLOT_COUNT];
    int      fds[SLOT_COUNT];
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        sizes[i] = gl.slots[i].size;
        fds[i]   = gl.slots[i].dmabuf_fd;
    }

    std::lock_guard<std::mutex> lock(s.send_mu);
    s.bind_generation += 1;

    ww_evt_bind_buffers_t bb {};
    bb.generation   = s.bind_generation;
    bb.flags        = s.flags;
    bb.count        = SLOT_COUNT;
    bb.fourcc       = gl.export_fourcc;
    bb.width        = opt.width;
    bb.height       = opt.height;
    bb.stride       = gl.slots[0].stride;
    bb.modifier     = gl.slots[0].drm_modifier;
    bb.plane_offset = gl.slots[0].offset;
    bb.sizes.count  = SLOT_COUNT;
    bb.sizes.data   = sizes;

    int rc = ww_bridge_send_bind_buffers(s.sock, &bb, fds);
    if (rc != 0) die("send bind_buffers failed: " + std::to_string(rc));
}

void send_frame(HostState& s, GlCtx& gl, uint32_t slot) {
    int sync_fd = export_sync_fd(gl);
    if (sync_fd < 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: eglDupNativeFenceFDANDROID failed\n");
        s.shutdown.store(true, std::memory_order_release);
        return;
    }

    ww_evt_frame_ready_t fr {};
    fr.image_index = slot;
    fr.seq         = s.seq.fetch_add(1, std::memory_order_relaxed);
    fr.ts_ns       = now_ns();

    int rc;
    {
        std::lock_guard<std::mutex> lock(s.send_mu);
        // Bump the release timeline under the send lock so concurrent
        // re-emits (e.g. apply_configure) can't race the counter.
        ++s.release_point;
        fr.release_point = s.release_point;
        s.last_release_point[slot] = s.release_point;
        rc = ww_bridge_send_frame_ready(s.sock, &fr, sync_fd);
    }
    // SCM_RIGHTS dup'd the fd on success; close our copy either way.
    close(sync_fd);
    if (rc != 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: send frame_ready failed: %d\n",
                     rc);
        s.shutdown.store(true, std::memory_order_release);
    }
}


// ---------------------------------------------------------------------------
// drm_syncobj export (for the release timeline)
// ---------------------------------------------------------------------------
//
// We don't pull libdrm into the plugin link surface; redefine the
// minimal kernel uAPI subset here. Layouts mirror <linux/drm.h>.

namespace {

struct WwDrmSyncobjCreate {
    uint32_t handle;
    uint32_t flags;
};
struct WwDrmSyncobjHandle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
    uint32_t pad;
};
struct WwDrmSyncobjDestroy {
    uint32_t handle;
    uint32_t pad;
};
struct WwDrmSyncobjTimelineWait {
    uint64_t handles;       // ptr to u32 array
    uint64_t points;        // ptr to u64 array
    int64_t  timeout_nsec;  // absolute CLOCK_MONOTONIC
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
};

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#define WW_DRM_IOCTL_SYNCOBJ_CREATE \
    _IOWR(DRM_IOCTL_BASE, 0xBF, WwDrmSyncobjCreate)
#define WW_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xC0, WwDrmSyncobjDestroy)
#define WW_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD \
    _IOWR(DRM_IOCTL_BASE, 0xC1, WwDrmSyncobjHandle)
#define WW_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT \
    _IOWR(DRM_IOCTL_BASE, 0xCA, WwDrmSyncobjTimelineWait)
#define WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1u << 0)
#define WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1u << 1)

// Create a fresh drm_syncobj on `drm_fd` and ship its OPAQUE_FD
// export over the bridge as a `ReleaseSyncobj` event. Caches the
// kernel handle on `s.release_syncobj_handle` so the destructor
// can DESTROY it. Returns true on success.
bool emit_release_syncobj(HostState& s, int drm_fd) {
    WwDrmSyncobjCreate cr {};
    if (::ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_CREATE, &cr) != 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: DRM_IOCTL_SYNCOBJ_CREATE failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    s.release_syncobj_handle = cr.handle;

    WwDrmSyncobjHandle h2fd {};
    h2fd.handle = cr.handle;
    h2fd.fd     = -1;
    if (::ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2fd) != 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    int rc;
    {
        std::lock_guard<std::mutex> lock(s.send_mu);
        rc = ww_bridge_send_release_syncobj(s.sock, h2fd.fd);
    }
    ::close(h2fd.fd);
    if (rc != 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: send release_syncobj failed: %d\n",
                     rc);
        return false;
    }
    return true;
}

void destroy_release_syncobj(HostState& s, int drm_fd) {
    if (s.release_syncobj_handle == 0 || drm_fd < 0) return;
    WwDrmSyncobjDestroy dst { s.release_syncobj_handle, 0 };
    (void)::ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_DESTROY, &dst);
    s.release_syncobj_handle = 0;
}

// Block until release_syncobj@point is signaled. Returns true on
// signal, false on timeout/ioctl error (caller logs and proceeds —
// running ahead of consumers is preferable to a stuck producer).
// `timeout_ms` is wall-clock from now.
bool wait_release_point(int drm_fd, uint32_t handle, uint64_t point,
                        unsigned int timeout_ms) {
    if (drm_fd < 0 || handle == 0 || point == 0) return true;
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return false;
    int64_t deadline = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000
                       + static_cast<int64_t>(ts.tv_nsec)
                       + static_cast<int64_t>(timeout_ms) * 1'000'000;

    uint32_t handles[1] = { handle };
    uint64_t points[1]  = { point };
    WwDrmSyncobjTimelineWait arg {};
    arg.handles       = reinterpret_cast<uintptr_t>(handles);
    arg.points        = reinterpret_cast<uintptr_t>(points);
    arg.timeout_nsec  = deadline;
    arg.count_handles = 1;
    // WAIT_FOR_SUBMIT lets us wait on a timeline `point` whose fence
    // hasn't been TRANSFERed in yet (the daemon's reaper races with the
    // renderer's next-frame back-pressure check). Without it the kernel
    // returns EINVAL immediately for any not-yet-materialized point.
    arg.flags         = WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
                      | WW_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;

    if (::ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg) != 0) {
        // ETIME = compositor still using buffer; we run ahead anyway
        // to avoid stalling mpv's clock. Other errors are logged at
        // the call site for visibility.
        return false;
    }
    return true;
}

} // namespace


// ---------------------------------------------------------------------------
// Control reader
// ---------------------------------------------------------------------------

void apply_control(HostState& s, MpvState& m, const Options& opt, GlCtx& gl,
                   const ww_bridge_control_t& c) {
    switch (c.op) {
    case WW_REQ_HELLO:
        break;
    case WW_REQ_LOAD_SCENE:
        if (c.u.load_scene.pkg && c.u.load_scene.pkg[0]) {
            const char* cmd[] = { "loadfile", c.u.load_scene.pkg, nullptr };
            mpv_command(m.mpv, cmd);
        }
        break;
    case WW_REQ_PLAY: {
        int v = 0;
        mpv_set_property(m.mpv, "pause", MPV_FORMAT_FLAG, &v);
        break;
    }
    case WW_REQ_PAUSE: {
        int v = 1;
        mpv_set_property(m.mpv, "pause", MPV_FORMAT_FLAG, &v);
        break;
    }
    case WW_REQ_MOUSE:
        // Videos don't respond to mouse input today.
        break;
    case WW_REQ_SET_FPS:
        // libmpv paces itself to the media's native frame rate.
        break;
    case WW_REQ_SHUTDOWN:
        s.shutdown.store(true, std::memory_order_release);
        break;
    case WW_REQ_CONFIGURE_BUFFERS:
        // GBM/LINEAR is intrinsically GTT (host-visible); we cannot
        // physically downgrade to DEVICE_LOCAL. Acknowledge the
        // request by re-emitting bind_buffers with a bumped generation
        // — the wire `flags` field will report what we actually have
        // (BUF_HOST_VISIBLE), and the daemon's reader thread will
        // clear pending_configure with a warn log if that doesn't
        // match what was asked for.
        if (c.u.configure_buffers.flags != s.flags) {
            std::fprintf(stderr,
                         "waywallen-mpv-renderer: ConfigureBuffers asked for "
                         "flags=0x%x but we can only do flags=0x%x (GBM LINEAR "
                         "→ GTT); answering with current placement\n",
                         c.u.configure_buffers.flags, s.flags);
        }
        send_bind(s, opt, gl);
        break;
    default:
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: unknown control op %d\n",
                     static_cast<int>(c.op));
        break;
    }
}

void reader_loop(HostState& s, MpvState& m, const Options& opt, GlCtx& gl,
                 WakeState& wake) {
    while (!s.shutdown.load(std::memory_order_acquire)) {
        ww_bridge_control_t msg {};
        int                 rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (!s.shutdown.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                             "waywallen-mpv-renderer: recv_control failed: %d\n",
                             rc);
            }
            s.shutdown.store(true, std::memory_order_release);
            wake_up(wake);
            return;
        }
        apply_control(s, m, opt, gl, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    if (opt.ipc_path.empty()) die("--ipc <socket_path> is required");

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    GlCtx gl;
    init_egl(gl, opt);

    WakeState wake;
    MpvState  mpv;
    mpv_init(mpv, opt, wake);

    HostState host;
    host.sock = ww_bridge_connect(opt.ipc_path.c_str());
    if (host.sock < 0)
        die("ww_bridge_connect: " + std::string(std::strerror(-host.sock)));

    // Resolve the DRM render-node major/minor from the GBM fd so the
    // daemon can match the renderer's GPU against each connected
    // display's GPU. fstat() on /dev/dri/renderD* gives `st_rdev`
    // whose major/minor split is what we report on the wire. fstat
    // failing is non-fatal — we'd report (0,0) and the daemon would
    // conservatively force HOST_VISIBLE on every connected display
    // (which we already are, so functionally a no-op).
    uint32_t drm_render_major = 0, drm_render_minor = 0;
    {
        struct stat st;
        if (gl.drm_fd >= 0 && ::fstat(gl.drm_fd, &st) == 0) {
            drm_render_major = major(st.st_rdev);
            drm_render_minor = minor(st.st_rdev);
        }
    }
    if (int rc = ww_bridge_send_ready(host.sock,
                                      drm_render_major, drm_render_minor);
        rc != 0)
        die("send ready failed: " + std::to_string(rc));
    std::fprintf(stderr,
                 "waywallen-mpv-renderer: ready drm_render=%u:%u\n",
                 drm_render_major, drm_render_minor);

    // Allocate the release timeline syncobj on the GBM-owned DRM fd
    // and send the OPAQUE_FD export to the daemon. Required before the
    // first frame_ready (which carries a release_point on this
    // timeline). If we can't allocate, the daemon's reaper will warn
    // and skip TRANSFERs — frames still flow, just no producer-side
    // back-pressure.
    if (gl.drm_fd >= 0 && !emit_release_syncobj(host, gl.drm_fd)) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: continuing without release_syncobj\n");
    } else if (gl.drm_fd < 0) {
        std::fprintf(stderr,
                     "waywallen-mpv-renderer: no DRM fd, skipping release_syncobj\n");
    }

    send_bind(host, opt, gl);

    std::thread reader([&]() { reader_loop(host, mpv, opt, gl, wake); });

    uint32_t slot = 0;
    while (!host.shutdown.load(std::memory_order_acquire)) {
        // Block until mpv signals a new update (or we're shutting down).
        // This replaces the old 5ms polling sleep and removes the external
        // fps gate entirely — pacing is libmpv's responsibility.
        {
            std::unique_lock<std::mutex> lk(wake.mu);
            wake.cv.wait(lk, [&] {
                return wake.pending
                       || host.shutdown.load(std::memory_order_acquire);
            });
            wake.pending = false;
        }
        if (host.shutdown.load(std::memory_order_acquire)) break;

        mpv_drain_events(mpv, host.shutdown);
        if (host.shutdown.load(std::memory_order_acquire)) break;

        const uint64_t update = mpv_render_context_update(mpv.ctx);
        if (!(update & MPV_RENDER_UPDATE_FRAME)) continue;

        // Producer back-pressure: wait until every consumer has
        // released the previous use of this slot before overwriting
        // the dma-buf. Skip on first rotation through the slots
        // (last_release_point[slot] == 0). Snapshot the point under
        // send_mu so a racing apply_configure doesn't tear it.
        uint64_t wait_point;
        {
            std::lock_guard<std::mutex> lock(host.send_mu);
            wait_point = host.last_release_point[slot];
        }
        if (wait_point > 0
            && !wait_release_point(gl.drm_fd, host.release_syncobj_handle,
                                   wait_point, /*timeout_ms=*/250)) {
            // Soft fail: log and proceed. The compositor MIGHT still
            // be reading the buffer, but waiting longer would stall
            // mpv's clock. The reaper's force-SIGNAL fallback also
            // covers this from the other side.
            std::fprintf(stderr,
                         "waywallen-mpv-renderer: release timeline wait timed out "
                         "at point %llu (slot %u), proceeding anyway\n",
                         (unsigned long long)wait_point, slot);
        }

        if (!mpv_render_into_slot(mpv, gl, slot, opt)) continue;

        send_frame(host, gl, slot);

        slot = (slot + 1) % SLOT_COUNT;
    }

    // --- Shutdown ---------------------------------------------------------
    // Flush any outstanding GL work before we tear mpv down.
    glFinish();

    if (mpv.ctx) mpv_render_context_free(mpv.ctx);
    if (mpv.mpv) mpv_terminate_destroy(mpv.mpv);

    if (reader.joinable()) {
        ::shutdown(host.sock, SHUT_RD);
        reader.join();
    }
    ww_bridge_close(host.sock);

    destroy_release_syncobj(host, gl.drm_fd);
    destroy_gl(gl);
    return 0;
}
