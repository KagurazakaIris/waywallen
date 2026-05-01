#pragma once

// waywallen::ffvk — the Vulkan-side helper layer that the renderer plugins
// share. Iter 1 lifts the per-plugin `VkProducer` here so the image and
// video plugins stop carrying duplicate copies. Iter 2 grows this header
// with FFmpeg-vulkan glue (AVHWDeviceContext, NV12→RGBA compute pass).
//
// The lib is plain C++20 (no rstd dep) so the plugins build tree can
// pull it in without dragging fetchdeps. The C++20-modules thumbnail
// extractor in `decoder.cppm` lives in a sibling target and is unaffected.

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

namespace waywallen::ffvk {

// Bring up a VkInstance/VkPhysicalDevice/VkDevice with the extension set
// the bridge pool's Vulkan backend needs (DMA-BUF export, modifier
// import, semaphore SYNC_FD), plus a HOST_VISIBLE|COHERENT staging
// buffer pre-mapped at `width*height*4` bytes for repeated RGBA8
// uploads.
//
// Iter 0/1 callers use upload_into() once per video frame — the per-
// submit fence inside makes that race-free against cmd buffer reuse.
// Iter 2's GPU YUV→RGB path reuses just the device/instance handles
// and drives its own command buffers; the staging buffer is then
// repurposed for sw-fallback uploads.
class Producer {
public:
    ~Producer();
    Producer(const Producer&)            = delete;
    Producer& operator=(const Producer&) = delete;

    static std::unique_ptr<Producer>
    create(uint32_t width, uint32_t height, std::string* err);

    // Iter 4 overload: pin the picked VkPhysicalDevice to the GPU that
    // exposes `render_node` (e.g. "/dev/dri/renderD128"). Requires
    // VK_EXT_physical_device_drm on the candidate device. Empty string
    // → behaves identically to the no-arg overload (first device that
    // advertises the required extension set wins). On mismatch the
    // function fails rather than silently picking another GPU; render-
    // node mismatch is a hard configuration error.
    static std::unique_ptr<Producer>
    create_with_render_node(uint32_t width, uint32_t height,
                            const std::string& render_node,
                            std::string* err);

    VkInstance       instance() const         { return instance_; }
    VkPhysicalDevice physical_device() const  { return phys_; }
    VkDevice         device() const           { return device_; }
    VkQueue          queue() const            { return queue_; }
    uint32_t         queue_family_index() const { return queue_family_; }
    uint32_t         drm_render_major() const { return drm_render_major_; }
    uint32_t         drm_render_minor() const { return drm_render_minor_; }
    const uint8_t*   device_uuid() const      { return have_uuid_ ? device_uuid_ : nullptr; }
    const uint8_t*   driver_uuid() const      { return have_uuid_ ? driver_uuid_ : nullptr; }
    int              drm_render_fd() const    { return drm_render_fd_; }
    uint32_t         width() const  { return width_; }
    uint32_t         height() const { return height_; }

    // Copy `data` (tightly packed RGBA8, `size` bytes == width*height*4)
    // into `target` VkImage and return an exported sync_fd that signals
    // when the GPU is done writing. The bridge pool takes ownership of
    // the sync_fd. Returns -1 with `*err` populated on failure.
    int upload_into(VkImage target, uint32_t target_width, uint32_t target_height,
                    const uint8_t* data, size_t size, std::string* err);

private:
    Producer() = default;

    VkInstance       instance_ { VK_NULL_HANDLE };
    VkPhysicalDevice phys_ { VK_NULL_HANDLE };
    VkDevice         device_ { VK_NULL_HANDLE };
    uint32_t         queue_family_ { 0 };
    VkQueue          queue_ { VK_NULL_HANDLE };

    VkCommandPool    cmd_pool_ { VK_NULL_HANDLE };
    VkCommandBuffer  cmd_ { VK_NULL_HANDLE };
    VkSemaphore      signal_sem_ { VK_NULL_HANDLE };
    VkFence          done_fence_ { VK_NULL_HANDLE };
    bool             fence_pending_ { false };

    VkBuffer         staging_buf_ { VK_NULL_HANDLE };
    VkDeviceMemory   staging_mem_ { VK_NULL_HANDLE };
    void*            staging_map_ { nullptr };
    VkDeviceSize     staging_size_ { 0 };

    uint32_t         width_ { 0 };
    uint32_t         height_ { 0 };
    uint32_t         drm_render_major_ { 0 };
    uint32_t         drm_render_minor_ { 0 };
    int              drm_render_fd_ { -1 };

    bool             have_uuid_ { false };
    uint8_t          device_uuid_[16] { 0 };
    uint8_t          driver_uuid_[16] { 0 };

    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR_ { nullptr };
};

} // namespace waywallen::ffvk
