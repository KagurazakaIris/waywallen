/* waywallen-bridge — DRM fourcc constants and helpers shared across
 * renderer subprocesses.
 *
 * The values are the canonical Linux DRM fourccs (see
 * <drm/drm_fourcc.h>); we redefine them locally so renderer
 * subprocesses don't need a libdrm/uapi include path. Only the
 * 32-bit RGBA family the renderers actually probe is listed.
 */
#ifndef WAYWALLEN_BRIDGE_DRM_FOURCC_H
#define WAYWALLEN_BRIDGE_DRM_FOURCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fourcc('A','B','2','4'): memory order R,G,B,A — matches GL_RGBA8
 * and Vulkan's VK_FORMAT_R8G8B8A8_UNORM channel layout on Mesa. The
 * canonical wire format used between waywallen renderer subprocesses
 * and the daemon. */
#define WW_DRM_FORMAT_ABGR8888 0x34324241u
#define WW_DRM_FORMAT_XBGR8888 0x34324258u
#define WW_DRM_FORMAT_ARGB8888 0x34325241u
#define WW_DRM_FORMAT_XRGB8888 0x34325258u
#define WW_DRM_FORMAT_RGBA8888 0x41424752u
#define WW_DRM_FORMAT_BGRA8888 0x41524742u
#define WW_DRM_FORMAT_RGBX8888 0x58424752u
#define WW_DRM_FORMAT_BGRX8888 0x58524742u

/* DRM_FORMAT_MOD_LINEAR — modifier value for "no tiling, just plain
 * row-major bytes". The kernel uapi defines this in
 * <drm/drm_fourcc.h>; some gbm.h pulls it in transitively, others
 * don't, so guard. */
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

/* Return a static, NUL-terminated short name for the supported
 * fourccs above. Unknown values yield "?". Useful for log messages
 * shared across renderers. */
const char *ww_fourcc_name(uint32_t fourcc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_DRM_FOURCC_H */
