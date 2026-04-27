#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <waywallen-bridge/protocol_bits.h>

namespace ww_image {

// Producer-side capability snapshot for the modifier-negotiation
// protocol (waywallen-ipc-v2 `format_caps` event). Encoded as parallel
// arrays mirroring the wire shape so we can hand the buffers straight
// to the bridge layer with no further allocation.
//
// Bit constants (USAGE_*, MEM_HINT_*, SYNC_*) live in
// `waywallen/src/negotiate.rs` — keep them in sync. The image renderer
// never asks for SYNC_* / COLOR_* / extent_max here; those are filled
// at the call site in main.cpp where they're closer to the IPC
// dispatch.
struct VkFormatCaps {
    std::vector<uint32_t> fourccs;
    std::vector<uint32_t> mod_counts;
    std::vector<uint64_t> modifiers;
    std::vector<uint32_t> usages;
    std::vector<uint32_t> plane_counts;
    uint8_t  device_uuid[16];
    uint8_t  driver_uuid[16];
    uint32_t mem_hints;
};

// Immutable view of the DMA-BUF backing a VkImage slot. Owned by VkProducer;
// the `dmabuf_fd` stays open for the producer's lifetime and is dup'd into
// SCM_RIGHTS messages by the IPC layer.
struct VkSlotLayout {
    int      dmabuf_fd { -1 };
    uint64_t drm_modifier { 0 };
    uint32_t drm_fourcc { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t plane_offset { 0 };
    uint32_t stride { 0 }; // bytes per row (rowPitch)
    uint32_t size { 0 };   // total memory size for this plane/image
};

// `WW_BUF_HOST_VISIBLE` is defined in <waywallen-bridge/protocol_bits.h>.
// Bit set on the daemon-controlled `ConfigureBuffers.flags` request and
// echoed back on `BindBuffers.flags`. Bit 0 = host_visible: back the
// dmabuf with HOST_VISIBLE && !DEVICE_LOCAL memory (GTT) so a foreign
// GPU's amdgpu/i915 driver can PRIME-import it. Cleared = use plain
// DEVICE_LOCAL (VRAM) for the zero-copy same-GPU path.

// Encapsulates a minimal Vulkan 1.1 instance+device set up for DMA-BUF export
// and a single VkImage slot. M3 will extend this with staging upload and
// signal-semaphore sync_fd export.
class VkProducer {
public:
    ~VkProducer();
    VkProducer(const VkProducer&)            = delete;
    VkProducer& operator=(const VkProducer&) = delete;

    // Create a producer with one `width` x `height` slot. `flags`
    // selects the dmabuf placement (see WW_BUF_HOST_VISIBLE).
    // `modifier` is the DRM format modifier the slot is allocated
    // with — pass `DRM_FORMAT_MOD_LINEAR` (0) for the universal
    // cross-vendor placement, or any modifier the daemon's
    // `negotiate_buffers` named (must be one of the entries
    // advertised in `query_format_caps`). On failure returns nullptr
    // and populates `*err`.
    static std::unique_ptr<VkProducer>
    create(uint32_t width, uint32_t height, uint32_t flags,
           uint64_t modifier, std::string* err);

    const VkSlotLayout& layout() const { return layout_; }

    // Current placement flags the slot was allocated with (the value
    // last passed to `create`/`rebuild`).
    uint32_t flags() const { return flags_; }

    // DRM render-node major/minor of the picked physical device. `(0, 0)`
    // when `VK_EXT_physical_device_drm` isn't advertised. Reported to
    // the daemon in `Ready` so it can match the renderer's GPU against
    // each connected display's GPU.
    uint32_t drm_render_major() const { return drm_render_major_; }
    uint32_t drm_render_minor() const { return drm_render_minor_; }

    // Vulkan instance + picked physical device. Exposed (read-only)
    // so callers can hand them to bridge-side helpers like
    // `ww_bridge_vk_log_gpu_info` that need direct Vulkan handles.
    VkInstance       instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return phys_; }

    // Tear down the current image+memory+exported fd and rebuild with a
    // new placement. Caller is responsible for re-uploading data
    // afterwards (the staging buffer is preserved). On success the new
    // `layout()` is the post-rebuild layout. Returns false and writes
    // `*err` on failure (instance/device are left intact, but the
    // image slot is gone — caller should treat that as terminal).
    bool rebuild(uint32_t flags, uint64_t modifier, std::string* err);

    // The DRM modifier the current slot was allocated with. Mirrors
    // `layout().drm_modifier` (which is set by Vulkan from the
    // requested list); kept as a separate accessor so callers can
    // compare without holding the layout reference.
    uint64_t modifier() const { return modifier_; }

    // Copy `data` (tightly packed RGBA8, `width*height*4` bytes) into the
    // slot's DMA-BUF via a staging buffer, transition the image layout to
    // GENERAL and release queue-family ownership to FOREIGN so the external
    // consumer can read it, then export a one-shot sync_file fd for the
    // signal. Caller owns the returned fd (sent via SCM_RIGHTS and then
    // closed). Returns -1 on failure and populates `*err`.
    //
    // `wait_release_point` is a value on the producer's release timeline
    // syncobj (see `export_release_syncobj_fd`). Pass 0 for the first
    // submit (no prior frame to gate on); pass the `release_point` of
    // the previous `frame_ready` for any subsequent submit that reuses
    // the same image slot. The submit blocks on `release_timeline_sem_
    // @ wait_release_point` so the GPU does not overwrite the dma-buf
    // until the daemon has transferred all consumers' release fences
    // onto that point.
    int upload_and_submit(const uint8_t* data, size_t size,
                          uint64_t wait_release_point, std::string* err);

    // Export the release timeline syncobj as an OPAQUE_FD. The fd is the
    // wire-side handle the daemon imports via `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE`
    // and `TRANSFER`s consumer release fences onto, point-by-point.
    // Send exactly once over IPC, after `Ready` and before any
    // `FrameReady`. Caller owns the returned fd. Returns -1 on failure.
    int export_release_syncobj_fd(std::string* err);

    // Probe the picked physical device for what fourcc + modifier
    // combinations the renderer can actually allocate, plus its
    // Vulkan device + driver UUIDs. Result is the wire-shape parallel
    // arrays the bridge's `ww_bridge_send_format_caps` expects.
    //
    // Today we probe just `DRM_FORMAT_ABGR8888` since that's the only
    // fourcc the renderer ever asks Vulkan for. Adding more fourccs
    // later is a matter of looping over a list at the top of
    // `query_format_caps`.
    bool query_format_caps(VkFormatCaps* out, std::string* err) const;

private:
    VkProducer() = default;

    VkInstance       instance_ { VK_NULL_HANDLE };
    VkPhysicalDevice phys_ { VK_NULL_HANDLE };
    VkDevice         device_ { VK_NULL_HANDLE };
    uint32_t         queue_family_ { 0 };
    VkQueue          queue_ { VK_NULL_HANDLE };
    VkImage          image_ { VK_NULL_HANDLE };
    VkDeviceMemory   memory_ { VK_NULL_HANDLE };

    VkCommandPool    cmd_pool_ { VK_NULL_HANDLE };
    VkCommandBuffer  cmd_ { VK_NULL_HANDLE };
    VkSemaphore      signal_sem_ { VK_NULL_HANDLE };
    // Timeline semaphore exported as OPAQUE_FD; on Mesa drivers this
    // round-trips through a drm_syncobj that the daemon manipulates via
    // DRM_IOCTL_SYNCOBJ_TRANSFER. Producer-side we only WAIT on it (the
    // daemon does the SIGNAL via TRANSFER). NVIDIA proprietary may not
    // support this round-trip.
    VkSemaphore      release_timeline_sem_ { VK_NULL_HANDLE };

    VkBuffer         staging_buf_ { VK_NULL_HANDLE };
    VkDeviceMemory   staging_mem_ { VK_NULL_HANDLE };
    void*            staging_map_ { nullptr };
    VkDeviceSize     staging_size_ { 0 };

    VkSlotLayout layout_ {};

    uint32_t flags_ { 0 };
    /* DRM modifier passed to the next `build_image()`; set by
     * `create()` / `rebuild()` and read by `build_image()`. Defaults
     * to LINEAR so a producer with no caller-supplied preference
     * still picks the universal cross-vendor placement. */
    uint64_t modifier_ { 0 /* DRM_FORMAT_MOD_LINEAR */ };
    uint32_t drm_render_major_ { 0 };
    uint32_t drm_render_minor_ { 0 };

    PFN_vkGetMemoryFdKHR                         vkGetMemoryFdKHR_ { nullptr };
    PFN_vkGetSemaphoreFdKHR                      vkGetSemaphoreFdKHR_ { nullptr };
    PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT_ { nullptr };
    PFN_vkGetPhysicalDeviceProperties2           vkGetPhysicalDeviceProperties2_ { nullptr };

    // Internal: allocate image+memory+export fd+layout for the current
    // queue-family on the existing device, using `flags_` to pick the
    // memory type. Caller (create/rebuild) must ensure the previous
    // image/memory/fd has been torn down before invoking.
    bool build_image(std::string* err);
};

} // namespace ww_image
