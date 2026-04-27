/* waywallen-bridge — fourcc helper implementation.
 *
 * Kept in its own translation unit so renderer subprocesses can pull
 * the helper without depending on the IPC/framing surface.
 */
#include <waywallen-bridge/drm_fourcc.h>

const char *ww_fourcc_name(uint32_t fourcc) {
    switch (fourcc) {
        case WW_DRM_FORMAT_ABGR8888: return "ABGR8888";
        case WW_DRM_FORMAT_XBGR8888: return "XBGR8888";
        case WW_DRM_FORMAT_ARGB8888: return "ARGB8888";
        case WW_DRM_FORMAT_XRGB8888: return "XRGB8888";
        case WW_DRM_FORMAT_RGBA8888: return "RGBA8888";
        case WW_DRM_FORMAT_BGRA8888: return "BGRA8888";
        case WW_DRM_FORMAT_RGBX8888: return "RGBX8888";
        case WW_DRM_FORMAT_BGRX8888: return "BGRX8888";
        default:                     return "?";
    }
}
