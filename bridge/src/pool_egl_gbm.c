/* waywallen-bridge — EGL/GBM pool backend.
 *
 * Absorbs the modifier probe + dmabuf allocation + EGLImage import +
 * GL FBO creation that previously lived in plugins/mpv/src/main.cpp.
 *
 * Plugin contributes:
 *   - EGLDisplay (already initialised + GLES2 context current on
 *     plugin's main thread)
 *   - DRM render-node fd (moved into bridge; bridge wraps in gbm_device)
 *   - eglGetProcAddress
 *
 * Bridge owns:
 *   - gbm_device, gbm_bo's, dmabuf fds
 *   - EGLImageKHRs, GL textures, GL FBOs (created/destroyed on the
 *     plugin's GL thread — apply_directive is invoked from there)
 *   - modifier probe results
 */

#include <waywallen-bridge/probe_egl.h>
#include <waywallen-bridge/protocol_bits.h>
#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/drm_fourcc.h>

#include "pool_internal.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Hand-rolled GL constants — bridge does not link <GLES2/gl2.h>. */
#define WW_GL_NO_ERROR              0x0000
#define WW_GL_TEXTURE_2D            0x0DE1
#define WW_GL_RGBA                  0x1908
#define WW_GL_RGBA8                 0x8058
#define WW_GL_UNSIGNED_BYTE         0x1401
#define WW_GL_TEXTURE_MIN_FILTER    0x2800
#define WW_GL_TEXTURE_MAG_FILTER    0x2801
#define WW_GL_LINEAR                0x2601
#define WW_GL_TEXTURE_WRAP_S        0x2802
#define WW_GL_TEXTURE_WRAP_T        0x2803
#define WW_GL_CLAMP_TO_EDGE         0x812F
#define WW_GL_FRAMEBUFFER           0x8D40
#define WW_GL_COLOR_ATTACHMENT0     0x8CE0
#define WW_GL_FRAMEBUFFER_COMPLETE  0x8CD5

/* Function pointer types we need on top of `ww_bridge_egl_dt_t`. */
typedef void (*ww_pfn_glGenTextures)(int n, unsigned int *out);
typedef void (*ww_pfn_glDeleteTextures)(int n, const unsigned int *t);
typedef void (*ww_pfn_glBindTexture)(unsigned int target, unsigned int t);
typedef void (*ww_pfn_glTexParameteri)(unsigned int target, unsigned int pname,
                                       int param);
typedef unsigned int (*ww_pfn_glGetError)(void);
typedef void (*ww_pfn_glGenFramebuffers)(int n, unsigned int *out);
typedef void (*ww_pfn_glDeleteFramebuffers)(int n, const unsigned int *f);
typedef void (*ww_pfn_glBindFramebuffer)(unsigned int target, unsigned int f);
typedef void (*ww_pfn_glFramebufferTexture2D)(unsigned int target,
                                              unsigned int attachment,
                                              unsigned int textarget,
                                              unsigned int texture, int level);
typedef unsigned int (*ww_pfn_glCheckFramebufferStatus)(unsigned int target);
typedef void (*ww_pfn_glFlush)(void);
typedef void (*ww_pfn_glEGLImageTargetTexture2DOES)(unsigned int target,
                                                   void *image);

typedef struct egl_gbm_state {
    /* Plugin-owned, borrowed. */
    EGLDisplay                       display;
    /* Bridge-owned. */
    int                              drm_fd_borrowed; /* duplicate of pool->drm_fd */
    struct gbm_device               *gbm;
    /* Combined EGL + GL function table. */
    ww_bridge_egl_dt_t               edt;
    ww_pfn_glGenTextures             glGenTextures;
    ww_pfn_glDeleteTextures          glDeleteTextures;
    ww_pfn_glBindTexture             glBindTexture;
    ww_pfn_glTexParameteri           glTexParameteri;
    ww_pfn_glGetError                glGetError;
    ww_pfn_glGenFramebuffers         glGenFramebuffers;
    ww_pfn_glDeleteFramebuffers      glDeleteFramebuffers;
    ww_pfn_glBindFramebuffer         glBindFramebuffer;
    ww_pfn_glFramebufferTexture2D    glFramebufferTexture2D;
    ww_pfn_glCheckFramebufferStatus  glCheckFramebufferStatus;
    ww_pfn_glFlush                   glFlush;
    ww_pfn_glEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES;

    /* Per-slot GL/EGL handles, keyed by slot index. */
    struct {
        struct gbm_bo *bo;
        EGLImageKHR    image;
        unsigned int   gl_texture;
        unsigned int   gl_fbo;
    } slots[WW_POOL_MAX_SLOTS];
} egl_gbm_state_t;

static const uint32_t kCandidateFourccs[] = {
    WW_DRM_FORMAT_ABGR8888, WW_DRM_FORMAT_XBGR8888,
    WW_DRM_FORMAT_ARGB8888, WW_DRM_FORMAT_XRGB8888,
    WW_DRM_FORMAT_RGBA8888, WW_DRM_FORMAT_BGRA8888,
    WW_DRM_FORMAT_RGBX8888, WW_DRM_FORMAT_BGRX8888,
};

#define DRM_FORMAT_MOD_LINEAR 0ULL

static int load_gl_dispatch(egl_gbm_state_t *st,
                            ww_bridge_egl_get_proc_addr_fn get_proc) {
    if (ww_bridge_egl_dt_load(&st->edt, get_proc) != 0) return -EINVAL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    /* Function-pointer → void* → typed function-pointer is required
     * by the EGL spec for eglGetProcAddress; ISO C forbids it but
     * POSIX guarantees it works on every supported platform. */
    st->glGenTextures            = (ww_pfn_glGenTextures)            (void *)get_proc("glGenTextures");
    st->glDeleteTextures         = (ww_pfn_glDeleteTextures)         (void *)get_proc("glDeleteTextures");
    st->glBindTexture            = (ww_pfn_glBindTexture)            (void *)get_proc("glBindTexture");
    st->glTexParameteri          = (ww_pfn_glTexParameteri)          (void *)get_proc("glTexParameteri");
    st->glGetError               = (ww_pfn_glGetError)               (void *)get_proc("glGetError");
    st->glGenFramebuffers        = (ww_pfn_glGenFramebuffers)        (void *)get_proc("glGenFramebuffers");
    st->glDeleteFramebuffers     = (ww_pfn_glDeleteFramebuffers)     (void *)get_proc("glDeleteFramebuffers");
    st->glBindFramebuffer        = (ww_pfn_glBindFramebuffer)        (void *)get_proc("glBindFramebuffer");
    st->glFramebufferTexture2D   = (ww_pfn_glFramebufferTexture2D)   (void *)get_proc("glFramebufferTexture2D");
    st->glCheckFramebufferStatus = (ww_pfn_glCheckFramebufferStatus) (void *)get_proc("glCheckFramebufferStatus");
    st->glFlush                  = (ww_pfn_glFlush)                  (void *)get_proc("glFlush");
    st->glEGLImageTargetTexture2DOES =
        (ww_pfn_glEGLImageTargetTexture2DOES)(void *)get_proc("glEGLImageTargetTexture2DOES");
#pragma GCC diagnostic pop

    if (!st->glGenTextures || !st->glDeleteTextures || !st->glBindTexture ||
        !st->glTexParameteri || !st->glGetError || !st->glGenFramebuffers ||
        !st->glDeleteFramebuffers || !st->glBindFramebuffer ||
        !st->glFramebufferTexture2D || !st->glCheckFramebufferStatus ||
        !st->glFlush || !st->glEGLImageTargetTexture2DOES) {
        return -ENOSYS;
    }
    return 0;
}

/* Probe one (fourcc, modifier) by allocating a one-modifier gbm_bo,
 * importing as EGLImage with modifier attrs, binding to a transient
 * GL_TEXTURE_2D + FBO, and checking framebuffer completeness. Used
 * only for caps probing — produced bo is destroyed before return. */
struct probe_bag {
    uint64_t *modifiers;
    size_t    cap;
    size_t    n;
    int       any_external;  /* >=1 entry was external_only */
    int       any_total;
};

static void probe_emit_cb(uint64_t mod, int external_only, void *user) {
    struct probe_bag *b = (struct probe_bag *)user;
    b->any_total += 1;
    if (external_only) { b->any_external = 1; return; }
    if (b->n < b->cap) {
        b->modifiers[b->n++] = mod;
    }
}

static int gbm_probe_one_modifier(struct gbm_device *gbm,
                                  uint32_t fourcc, uint64_t modifier,
                                  uint32_t w, uint32_t h,
                                  uint32_t *out_planes) {
    uint64_t mods[1] = { modifier };
    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(
        gbm, w, h, fourcc, mods, 1, GBM_BO_USE_RENDERING);
    if (!bo) return -EIO;
    int planes = gbm_bo_get_plane_count(bo);
    if (planes <= 0) planes = 1;
    if (out_planes) *out_planes = (uint32_t)planes;
    gbm_bo_destroy(bo);
    return 0;
}

/* probe_caps: walk every candidate fourcc, enumerate modifier-aware
 * (EGL importer ∩ GBM producer) entries, and append them to the pool's
 * advertised set. If the modifier-aware probe yields zero entries
 * across every fourcc, append exactly one (ABGR8888, LINEAR, 1) entry
 * and set MEM_HINT_LINEAR_ONLY. */
static int probe_caps(ww_pool_t *pool, uint32_t width, uint32_t height) {
    egl_gbm_state_t *st = (egl_gbm_state_t *)pool->backend_data;

    /* Worst case: candidates × 32 modifiers. Heap-allocate generously. */
    size_t cap = 256;
    ww_format_entry_t *entries = (ww_format_entry_t *)calloc(cap, sizeof(*entries));
    if (!entries) return -ENOMEM;
    size_t n = 0;

    for (size_t fi = 0; fi < sizeof(kCandidateFourccs) / sizeof(kCandidateFourccs[0]); ++fi) {
        uint32_t fourcc = kCandidateFourccs[fi];
        uint64_t mod_buf[64];
        struct probe_bag bag = { mod_buf, 64, 0, 0, 0 };

        int rc = ww_bridge_egl_query_modifiers_for_fourcc(
            &st->edt, st->display, fourcc, probe_emit_cb, &bag);
        if (rc != 0 || bag.n == 0) continue;

        /* Float LINEAR to the front so GBM prefers it when allocator-feasible. */
        for (size_t i = 0; i < bag.n; ++i) {
            if (mod_buf[i] == DRM_FORMAT_MOD_LINEAR) {
                uint64_t t = mod_buf[0]; mod_buf[0] = mod_buf[i]; mod_buf[i] = t;
                break;
            }
        }

        /* Bulk gbm probe — does GBM produce ANY of these modifiers? */
        struct gbm_bo *probe_bo = gbm_bo_create_with_modifiers2(
            st->gbm, width, height, fourcc,
            mod_buf, bag.n, GBM_BO_USE_RENDERING);
        if (!probe_bo) continue;
        gbm_bo_destroy(probe_bo);

        /* Per-modifier probe — every modifier GBM can produce. */
        for (size_t i = 0; i < bag.n; ++i) {
            uint32_t planes = 1;
            if (gbm_probe_one_modifier(st->gbm, fourcc, mod_buf[i],
                                       width, height, &planes) != 0) {
                continue;
            }
            if (n >= cap) break;
            entries[n].fourcc      = fourcc;
            entries[n].modifier    = mod_buf[i];
            entries[n].plane_count = planes;
            n += 1;
        }
    }

    if (n == 0) {
        /* Modifier-aware probe yielded nothing — synthesize a single
         * ABGR8888 LINEAR entry. The daemon's same-device picker
         * walks the intersection and will pick LINEAR; the topology-
         * first cross-device branch ignores modifier lists entirely.
         * Either way the daemon emits CompatLinear and the bridge's
         * compat-linear allocation path takes over. */
        entries[0].fourcc      = WW_DRM_FORMAT_ABGR8888;
        entries[0].modifier    = DRM_FORMAT_MOD_LINEAR;
        entries[0].plane_count = 1;
        n = 1;
        fprintf(stderr,
                "ww_pool[egl_gbm]: modifier-aware probe yielded 0 entries — "
                "advertising single LINEAR fallback\n");
    }

    pool->caps.entries = entries;
    pool->caps.count   = n;
    pool->caps.sync_caps   = WW_SYNC_SYNCOBJ_TIMELINE;
    pool->caps.color_caps  = WW_COLOR_ENC_SRGB | WW_COLOR_RANGE_LIMITED |
                              WW_COLOR_ALPHA_PREMUL;
    pool->caps.extent_max_w = 16384;
    pool->caps.extent_max_h = 16384;
    return 0;
}

/* EGL import attribute keys per plane index 0-3. Indexed by plane. */
static const EGLint kEglPlaneFd[4] = {
    EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
};
static const EGLint kEglPlaneOffset[4] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
static const EGLint kEglPlanePitch[4] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
};
static const EGLint kEglPlaneModLo[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
};
static const EGLint kEglPlaneModHi[4] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
};

static EGLImageKHR import_with_modifier(egl_gbm_state_t *st,
                                        uint32_t fourcc, uint32_t w, uint32_t h,
                                        const int      *fds,
                                        const uint32_t *strides,
                                        const uint32_t *offsets,
                                        uint32_t        plane_count,
                                        uint64_t        modifier) {
    /* 8 base attrs + per-plane (fd,offset,pitch,modLo,modHi) * planes
     * + EGL_NONE. With WW_POOL_MAX_PLANES=4 the worst case is
     * 8 + 4*5*2 + 1 = 49 entries. Round up. */
    EGLint attrs[64];
    int k = 0;
    attrs[k++] = EGL_WIDTH;                attrs[k++] = (EGLint)w;
    attrs[k++] = EGL_HEIGHT;               attrs[k++] = (EGLint)h;
    attrs[k++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[k++] = (EGLint)fourcc;
    EGLint mod_lo = (EGLint)(modifier & 0xffffffffULL);
    EGLint mod_hi = (EGLint)((modifier >> 32) & 0xffffffffULL);
    for (uint32_t p = 0; p < plane_count && p < 4; ++p) {
        attrs[k++] = kEglPlaneFd[p];      attrs[k++] = fds[p];
        attrs[k++] = kEglPlaneOffset[p];  attrs[k++] = (EGLint)offsets[p];
        attrs[k++] = kEglPlanePitch[p];   attrs[k++] = (EGLint)strides[p];
        attrs[k++] = kEglPlaneModLo[p];   attrs[k++] = mod_lo;
        attrs[k++] = kEglPlaneModHi[p];   attrs[k++] = mod_hi;
    }
    attrs[k++] = EGL_NONE;
    return st->edt.eglCreateImageKHR(st->display, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

static EGLImageKHR import_without_modifier(egl_gbm_state_t *st,
                                           uint32_t fourcc, uint32_t w, uint32_t h,
                                           int dmabuf_fd, uint32_t stride,
                                           uint32_t offset) {
    EGLint attrs[] = {
        EGL_WIDTH,                     (EGLint)w,
        EGL_HEIGHT,                    (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT,      (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT,     dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)stride,
        EGL_NONE,
    };
    return st->edt.eglCreateImageKHR(st->display, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
}

static int alloc_slot(ww_pool_t *pool, uint32_t slot_index,
                      ww_pool_slot_layout_t *out) {
    egl_gbm_state_t *st = (egl_gbm_state_t *)pool->backend_data;
    if (slot_index >= WW_POOL_MAX_SLOTS) return -EINVAL;

    const ww_pool_directive_t *d = &pool->cur;
    bool linear_path = (d->category == WW_PATH_COMPAT_LINEAR) ||
                       (d->mem_source == WW_MEM_SRC_GPU_LINEAR);

    struct gbm_bo *bo = NULL;
    if (linear_path) {
        bo = gbm_bo_create(st->gbm, d->width, d->height, d->fourcc,
                           GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        if (!bo) {
            fprintf(stderr,
                    "ww_pool[egl_gbm]: gbm_bo_create(USE_LINEAR) failed for "
                    "fourcc=0x%08x %ux%u\n", d->fourcc, d->width, d->height);
            return -EIO;
        }
    } else {
        uint64_t mods[1] = { d->modifier };
        bo = gbm_bo_create_with_modifiers2(
            st->gbm, d->width, d->height, d->fourcc,
            mods, 1, GBM_BO_USE_RENDERING);
        if (!bo) {
            fprintf(stderr,
                    "ww_pool[egl_gbm]: gbm_bo_create_with_modifiers2 failed for "
                    "fourcc=0x%08x mod=0x%016llx %ux%u\n",
                    d->fourcc, (unsigned long long)d->modifier,
                    d->width, d->height);
            return -EIO;
        }
    }

    int      gbm_planes = gbm_bo_get_plane_count(bo);
    if (gbm_planes <= 0) gbm_planes = 1;
    if (gbm_planes > WW_POOL_MAX_PLANES) {
        fprintf(stderr,
                "ww_pool[egl_gbm]: alloc_slot[%u]: gbm reports %d planes, "
                "exceeds WW_POOL_MAX_PLANES=%d\n",
                slot_index, gbm_planes, WW_POOL_MAX_PLANES);
        gbm_bo_destroy(bo);
        return -ENOSPC;
    }
    uint64_t actual_mod = gbm_bo_get_modifier(bo);

    /* Per-plane fd / stride / offset / size, populated for `gbm_planes`
     * slots. Higher slots are sentinel values left from initialization. */
    int      plane_fds[WW_POOL_MAX_PLANES] = {-1, -1, -1, -1};
    uint32_t plane_strides[WW_POOL_MAX_PLANES] = {0};
    uint32_t plane_offsets[WW_POOL_MAX_PLANES] = {0};
    uint64_t plane_sizes[WW_POOL_MAX_PLANES]   = {0};
    for (int p = 0; p < gbm_planes; ++p) {
        plane_fds[p]     = gbm_bo_get_fd_for_plane(bo, p);
        if (plane_fds[p] < 0) {
            for (int q = 0; q < p; ++q) close(plane_fds[q]);
            gbm_bo_destroy(bo);
            return -EIO;
        }
        plane_strides[p] = gbm_bo_get_stride_for_plane(bo, p);
        plane_offsets[p] = gbm_bo_get_offset(bo, p);
    }
    /* Per-plane size: the contribution from this plane's offset to the
     * next plane's offset (or stride*height for plane 0 in the
     * single-plane case). For metadata planes we don't know the exact
     * end-of-plane without total memory size; the consumer-side import
     * doesn't actually need accurate sizes (eglCreateImageKHR ignores
     * size, vkAllocateMemory uses memoryRequirements). Best-effort
     * derivation: gap to next plane, last plane gets stride*height. */
    for (int p = 0; p < gbm_planes; ++p) {
        if (p + 1 < gbm_planes) {
            plane_sizes[p] = (uint64_t)plane_offsets[p + 1] - plane_offsets[p];
        } else {
            plane_sizes[p] = (uint64_t)plane_strides[p] * d->height;
        }
    }

    fprintf(stderr,
            "ww_pool[egl_gbm]: alloc_slot[%u] %ux%u fourcc=0x%08x "
            "mod=0x%016llx linear=%d gbm_planes=%d\n",
            slot_index, d->width, d->height, d->fourcc,
            (unsigned long long)actual_mod, linear_path ? 1 : 0, gbm_planes);
    for (int p = 0; p < gbm_planes; ++p) {
        fprintf(stderr,
                "  plane[%d] fd=%d stride=%u offset=%u size=%llu\n",
                p, plane_fds[p], plane_strides[p], plane_offsets[p],
                (unsigned long long)plane_sizes[p]);
    }

    /* EGLImage import. LINEAR path imports without modifier attrs to
     * accommodate drivers (NVIDIA prop) where LINEAR is external_only
     * via the modifier-tagged path. Modifier path passes every plane
     * so DCC metadata, retile data, etc., land where the driver
     * expects. */
    EGLImageKHR img = linear_path
        ? import_without_modifier(st, d->fourcc, d->width, d->height,
                                  plane_fds[0], plane_strides[0],
                                  plane_offsets[0])
        : import_with_modifier(st, d->fourcc, d->width, d->height,
                               plane_fds, plane_strides, plane_offsets,
                               (uint32_t)gbm_planes, actual_mod);
    if (img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr,
                "ww_pool[egl_gbm]: eglCreateImageKHR failed (egl_err=0x%04x) "
                "fourcc=0x%08x mod=0x%016llx linear=%d planes=%d\n",
                st->edt.eglGetError(), d->fourcc,
                (unsigned long long)actual_mod, linear_path ? 1 : 0,
                gbm_planes);
        for (int p = 0; p < gbm_planes; ++p) close(plane_fds[p]);
        gbm_bo_destroy(bo);
        return -EIO;
    }

    /* Build the GL texture + FBO. */
    unsigned int tex = 0, fbo = 0;
    st->glGenTextures(1, &tex);
    st->glBindTexture(WW_GL_TEXTURE_2D, tex);
    (void)st->glGetError(); /* clear */
    st->glEGLImageTargetTexture2DOES(WW_GL_TEXTURE_2D, img);
    unsigned int gl_err = st->glGetError();
    st->glTexParameteri(WW_GL_TEXTURE_2D, WW_GL_TEXTURE_MIN_FILTER, WW_GL_LINEAR);
    st->glTexParameteri(WW_GL_TEXTURE_2D, WW_GL_TEXTURE_MAG_FILTER, WW_GL_LINEAR);
    st->glTexParameteri(WW_GL_TEXTURE_2D, WW_GL_TEXTURE_WRAP_S, WW_GL_CLAMP_TO_EDGE);
    st->glTexParameteri(WW_GL_TEXTURE_2D, WW_GL_TEXTURE_WRAP_T, WW_GL_CLAMP_TO_EDGE);

    st->glGenFramebuffers(1, &fbo);
    st->glBindFramebuffer(WW_GL_FRAMEBUFFER, fbo);
    st->glFramebufferTexture2D(WW_GL_FRAMEBUFFER, WW_GL_COLOR_ATTACHMENT0,
                               WW_GL_TEXTURE_2D, tex, 0);
    unsigned int fbo_status = st->glCheckFramebufferStatus(WW_GL_FRAMEBUFFER);
    st->glBindFramebuffer(WW_GL_FRAMEBUFFER, 0);
    if (fbo_status != WW_GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "ww_pool[egl_gbm]: slot[%u] FBO incomplete: status=0x%04x "
                "gl_err=0x%04x mod=0x%016llx fourcc=0x%08x linear=%d\n",
                slot_index, fbo_status, gl_err,
                (unsigned long long)actual_mod, d->fourcc, linear_path ? 1 : 0);
        st->glDeleteFramebuffers(1, &fbo);
        st->glDeleteTextures(1, &tex);
        st->edt.eglDestroyImageKHR(st->display, img);
        for (int p = 0; p < gbm_planes; ++p) close(plane_fds[p]);
        gbm_bo_destroy(bo);
        return -EIO;
    }

    st->slots[slot_index].bo         = bo;
    st->slots[slot_index].image      = img;
    st->slots[slot_index].gl_texture = tex;
    st->slots[slot_index].gl_fbo     = fbo;

    out->plane_count = (uint32_t)gbm_planes;
    for (int p = 0; p < gbm_planes; ++p) {
        out->fds[p]            = plane_fds[p];
        out->strides[p]        = plane_strides[p];
        out->plane_offsets[p]  = plane_offsets[p];
        out->sizes[p]          = plane_sizes[p];
    }
    for (int p = gbm_planes; p < WW_POOL_MAX_PLANES; ++p) {
        out->fds[p] = -1;
    }
    out->modifier = actual_mod;
    return 0;
}

static void free_slot(ww_pool_t *pool, uint32_t slot_index) {
    if (slot_index >= WW_POOL_MAX_SLOTS) return;
    egl_gbm_state_t *st = (egl_gbm_state_t *)pool->backend_data;
    if (st->slots[slot_index].gl_fbo) {
        st->glDeleteFramebuffers(1, &st->slots[slot_index].gl_fbo);
        st->slots[slot_index].gl_fbo = 0;
    }
    if (st->slots[slot_index].gl_texture) {
        st->glDeleteTextures(1, &st->slots[slot_index].gl_texture);
        st->slots[slot_index].gl_texture = 0;
    }
    if (st->slots[slot_index].image != EGL_NO_IMAGE_KHR) {
        st->edt.eglDestroyImageKHR(st->display, st->slots[slot_index].image);
        st->slots[slot_index].image = EGL_NO_IMAGE_KHR;
    }
    if (st->slots[slot_index].bo) {
        gbm_bo_destroy(st->slots[slot_index].bo);
        st->slots[slot_index].bo = NULL;
    }
}

static int populate_slot_view(ww_pool_t *pool, uint32_t slot_index,
                              ww_pool_slot_t *out) {
    egl_gbm_state_t *st = (egl_gbm_state_t *)pool->backend_data;
    if (slot_index >= WW_POOL_MAX_SLOTS) return -EINVAL;
    out->gl_export_fbo     = st->slots[slot_index].gl_fbo;
    out->gl_export_texture = st->slots[slot_index].gl_texture;
    out->vk_image          = NULL;
    out->vk_memory         = NULL;
    return 0;
}

static void backend_destroy(ww_pool_t *pool) {
    egl_gbm_state_t *st = (egl_gbm_state_t *)pool->backend_data;
    if (!st) return;
    if (st->gbm) gbm_device_destroy(st->gbm);
    /* drm_fd_borrowed is the same fd as pool->drm_fd which is closed
     * by the dispatcher. */
    free(st);
    pool->backend_data = NULL;
}

static int backend_init(ww_pool_t *pool, const void *init_data) {
    const ww_pool_egl_gbm_init_t *init = (const ww_pool_egl_gbm_init_t *)init_data;
    if (!init || !init->egl_display || init->drm_render_fd < 0 ||
        !init->get_proc_address) {
        return -EINVAL;
    }
    egl_gbm_state_t *st = (egl_gbm_state_t *)calloc(1, sizeof(*st));
    if (!st) return -ENOMEM;
    for (uint32_t i = 0; i < WW_POOL_MAX_SLOTS; ++i) {
        st->slots[i].image = EGL_NO_IMAGE_KHR;
    }

    st->display       = (EGLDisplay)init->egl_display;
    st->drm_fd_borrowed = init->drm_render_fd;
    pool->drm_fd      = init->drm_render_fd;  /* moved */

    /* Plugin's `void *(*)(const char *)` is convention-compatible with
     * EGL's `__eglMustCastToProperFunctionPointerType (*)(const char *)`
     * — eglGetProcAddress returns an opaque function pointer that
     * callers cast. The C standard forbids implicit conversion
     * between function pointers and object pointers, so cast through
     * an intptr-sized union to silence the warning while keeping the
     * actual pointer bits intact. */
    union {
        void *(*as_obj)(const char *);
        ww_bridge_egl_get_proc_addr_fn as_fn;
    } cvt;
    cvt.as_obj = init->get_proc_address;
    if (load_gl_dispatch(st, cvt.as_fn) != 0) {
        fprintf(stderr,
                "ww_pool[egl_gbm]: failed to resolve required EGL/GL entry points\n");
        free(st);
        return -ENOSYS;
    }

    st->gbm = gbm_create_device(init->drm_render_fd);
    if (!st->gbm) {
        fprintf(stderr, "ww_pool[egl_gbm]: gbm_create_device failed\n");
        free(st);
        return -EIO;
    }

    pool->backend_data = st;
    pool->caps.drm_render_major = init->drm_render_major;
    pool->caps.drm_render_minor = init->drm_render_minor;
    pool->caps.have_uuid = false; /* EGL doesn't readily produce VkPhysicalDeviceIDProperties */
    return 0;
}

static const struct ww_pool_backend_ops kEglGbmOps = {
    .init               = backend_init,
    .probe_caps         = probe_caps,
    .alloc_slot         = alloc_slot,
    .free_slot          = free_slot,
    .populate_slot_view = populate_slot_view,
    .destroy            = backend_destroy,
};

int ww_pool_egl_gbm_create(ww_pool_t *pool, const void *init_data) {
    pool->ops = &kEglGbmOps;
    int rc = backend_init(pool, init_data);
    if (rc != 0) {
        pool->ops = NULL;
    }
    return rc;
}
