/* waywallen-bridge — Vulkan pool backend.
 *
 * Owns: per-slot VkImage + VkDeviceMemory + dmabuf fd. Bridge does
 * not link libvulkan — function pointers come from the plugin's
 * vkGetInstanceProcAddr.
 *
 * Plugin still owns: VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
 * its own staging buffer + command pool. Plugin uploads pixels into
 * the slot's VkImage by issuing transfer commands targeting the
 * VkImage handle returned from `populate_slot_view`. */

#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/protocol_bits.h>
#include <waywallen-bridge/probe_vk.h>
#include <waywallen-bridge/drm_fourcc.h>

#include "pool_internal.h"
#include "sync_release.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRM_FORMAT_MOD_LINEAR 0ULL

typedef struct vk_state {
    /* Plugin-owned. */
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         queue_family;

    /* Resolved through vkGetInstanceProcAddr / vkGetDeviceProcAddr. */
    PFN_vkGetInstanceProcAddr                       vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr                         vkGetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties2              vkGetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties2        vkGetPhysicalDeviceFormatProperties2;
    PFN_vkGetPhysicalDeviceMemoryProperties         vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCreateImage                               vkCreateImage;
    PFN_vkDestroyImage                              vkDestroyImage;
    PFN_vkGetImageMemoryRequirements2               vkGetImageMemoryRequirements2;
    PFN_vkAllocateMemory                            vkAllocateMemory;
    PFN_vkFreeMemory                                vkFreeMemory;
    PFN_vkBindImageMemory                           vkBindImageMemory;
    PFN_vkGetImageSubresourceLayout                 vkGetImageSubresourceLayout;
    PFN_vkGetMemoryFdKHR                            vkGetMemoryFdKHR;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT    vkGetImageDrmFormatModifierPropertiesEXT;
    PFN_vkDeviceWaitIdle                            vkDeviceWaitIdle;

    /* Per-slot resources. */
    struct {
        VkImage         image;
        VkDeviceMemory  memory;
    } slots[WW_POOL_MAX_SLOTS];
} vk_state_t;

static int load_dispatch(vk_state_t *st,
                         PFN_vkGetInstanceProcAddr getipa) {
    st->vkGetInstanceProcAddr = getipa;
#define LOAD_INSTANCE(name) \
    st->name = (PFN_##name)getipa(st->instance, #name)
    LOAD_INSTANCE(vkGetDeviceProcAddr);
    LOAD_INSTANCE(vkGetPhysicalDeviceProperties2);
    LOAD_INSTANCE(vkGetPhysicalDeviceFormatProperties2);
    LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
#undef LOAD_INSTANCE
    if (!st->vkGetDeviceProcAddr) return -ENOSYS;

#define LOAD_DEVICE(name) \
    st->name = (PFN_##name)st->vkGetDeviceProcAddr(st->device, #name)
    LOAD_DEVICE(vkCreateImage);
    LOAD_DEVICE(vkDestroyImage);
    LOAD_DEVICE(vkGetImageMemoryRequirements2);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkBindImageMemory);
    LOAD_DEVICE(vkGetImageSubresourceLayout);
    LOAD_DEVICE(vkGetMemoryFdKHR);
    LOAD_DEVICE(vkGetImageDrmFormatModifierPropertiesEXT);
    LOAD_DEVICE(vkDeviceWaitIdle);
#undef LOAD_DEVICE

    if (!st->vkCreateImage || !st->vkDestroyImage ||
        !st->vkGetImageMemoryRequirements2 || !st->vkAllocateMemory ||
        !st->vkFreeMemory || !st->vkBindImageMemory ||
        !st->vkGetImageSubresourceLayout || !st->vkGetMemoryFdKHR ||
        !st->vkGetImageDrmFormatModifierPropertiesEXT ||
        !st->vkGetPhysicalDeviceFormatProperties2 ||
        !st->vkGetPhysicalDeviceMemoryProperties) {
        return -ENOSYS;
    }
    return 0;
}

static uint32_t map_format_features_to_usage(VkFormatFeatureFlags ff) {
    uint32_t u = 0;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)        u |= WW_USAGE_SAMPLED;
    if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)        u |= WW_USAGE_STORAGE;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)     u |= WW_USAGE_COLOR_ATTACHMENT;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)         u |= WW_USAGE_TRANSFER_SRC;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)         u |= WW_USAGE_TRANSFER_DST;
    return u;
}

static int probe_caps(ww_pool_t *pool, uint32_t width, uint32_t height) {
    (void)width; (void)height; /* Vulkan modifiers don't depend on extent at probe time. */
    vk_state_t *st = (vk_state_t *)pool->backend_data;

    /* Two-call enumeration of modifier list for ABGR8888. */
    VkDrmFormatModifierPropertiesListEXT mod_list = {0};
    mod_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp2 = {0};
    fp2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp2.pNext = &mod_list;
    st->vkGetPhysicalDeviceFormatProperties2(
        st->phys, VK_FORMAT_R8G8B8A8_UNORM, &fp2);

    if (mod_list.drmFormatModifierCount == 0) {
        fprintf(stderr,
                "ww_pool[vulkan]: driver reports 0 modifiers for ABGR8888 — "
                "advertising single LINEAR fallback\n");
        ww_format_entry_t *ent = (ww_format_entry_t *)calloc(1, sizeof(*ent));
        if (!ent) return -ENOMEM;
        ent[0].fourcc      = WW_DRM_FORMAT_ABGR8888;
        ent[0].modifier    = DRM_FORMAT_MOD_LINEAR;
        ent[0].plane_count = 1;
        pool->caps.entries = ent;
        pool->caps.count   = 1;
        goto fill_secondary;
    }

    VkDrmFormatModifierPropertiesEXT *probed =
        (VkDrmFormatModifierPropertiesEXT *)calloc(
            mod_list.drmFormatModifierCount, sizeof(*probed));
    if (!probed) return -ENOMEM;
    mod_list.pDrmFormatModifierProperties = probed;
    st->vkGetPhysicalDeviceFormatProperties2(
        st->phys, VK_FORMAT_R8G8B8A8_UNORM, &fp2);

    ww_format_entry_t *entries = (ww_format_entry_t *)calloc(
        mod_list.drmFormatModifierCount, sizeof(*entries));
    if (!entries) { free(probed); return -ENOMEM; }

    for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; ++i) {
        entries[i].fourcc      = WW_DRM_FORMAT_ABGR8888;
        entries[i].modifier    = probed[i].drmFormatModifier;
        entries[i].plane_count = probed[i].drmFormatModifierPlaneCount;
        /* usages collected in flatten by reading the entry's modifier
         * back through the format props — but we discard per-modifier
         * usage and just emit SAMPLED on advertise. The format-feature
         * mask is logged for debugging. */
        (void)map_format_features_to_usage;
    }
    pool->caps.entries = entries;
    pool->caps.count   = mod_list.drmFormatModifierCount;
    free(probed);

fill_secondary: ;
    /* Fill device/driver UUID. */
    if (st->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties id_props = {0};
        id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 pd2 = {0};
        pd2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        pd2.pNext = &id_props;
        st->vkGetPhysicalDeviceProperties2(st->phys, &pd2);
        memcpy(pool->caps.device_uuid, id_props.deviceUUID, 16);
        memcpy(pool->caps.driver_uuid, id_props.driverUUID, 16);
        pool->caps.have_uuid = true;
    }

    pool->caps.sync_caps   = WW_SYNC_SYNCOBJ_TIMELINE;
    pool->caps.color_caps  = WW_COLOR_ENC_SRGB | WW_COLOR_RANGE_LIMITED |
                              WW_COLOR_ALPHA_PREMUL;
    pool->caps.extent_max_w = 16384;
    pool->caps.extent_max_h = 16384;
    return 0;
}

/* Pick a memory type from the producer device that can back an
 * exportable VkImage. Cross-GPU PRIME translates regardless of
 * HOST_VISIBLE / DEVICE_LOCAL — the only correctness requirement is
 * dma-buf exportability, which is enforced at vkCreateImage time
 * via VkExternalMemoryImageCreateInfo.handleTypes = DMA_BUF_BIT_EXT.
 * Prefer DEVICE_LOCAL when allowed (faster), else any matching type.
 * Returns UINT32_MAX only if typeBits is empty. */
static uint32_t pick_memory_type(vk_state_t *st, uint32_t type_bits) {
    VkPhysicalDeviceMemoryProperties mp = {0};
    st->vkGetPhysicalDeviceMemoryProperties(st->phys, &mp);
    /* Pass 1: prefer DEVICE_LOCAL. */
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            return i;
        }
    }
    /* Pass 2: any allowed type. */
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (type_bits & (1u << i)) return i;
    }
    return UINT32_MAX;
}

static int alloc_slot(ww_pool_t *pool, uint32_t slot_index,
                      ww_pool_slot_layout_t *out) {
    vk_state_t *st = (vk_state_t *)pool->backend_data;
    if (slot_index >= WW_POOL_MAX_SLOTS) return -EINVAL;

    const ww_pool_directive_t *d = &pool->cur;

    /* For the COMPAT_LINEAR path we override the modifier to LINEAR
     * regardless of what the directive says, since GPU_LINEAR is the
     * Vulkan analogue of `gbm_bo_create(USE_LINEAR)`. For OPTIMIZED
     * paths we use whatever modifier the daemon picked. */
    bool linear_path = (d->category == WW_PATH_COMPAT_LINEAR) ||
                       (d->mem_source == WW_MEM_SRC_GPU_LINEAR);
    uint64_t modifiers[1] = {
        linear_path ? DRM_FORMAT_MOD_LINEAR : d->modifier
    };

    VkImageDrmFormatModifierListCreateInfoEXT mod_list = {0};
    mod_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    mod_list.drmFormatModifierCount = 1;
    mod_list.pDrmFormatModifiers    = modifiers;

    VkExternalMemoryImageCreateInfo ext_img = {0};
    ext_img.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.pNext       = &mod_list;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo img_ci = {0};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext         = &ext_img;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent.width  = d->width;
    img_ci.extent.height = d->height;
    img_ci.extent.depth  = 1;
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = 1;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    /* Match the consumer's `TRANSFER_SRC_BIT` exactly (see
     * waywallen-display/src/backend_vulkan.c). For
     * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, the modifier alone does
     * NOT pin the DCC sub-layout — the driver also picks compression
     * block size / metadata swizzle from the usage flags. SAMPLED here
     * makes the driver lay out DCC for sampler reads, which the
     * TRANSFER_SRC-only consumer can't decompress, producing tile-grid
     * repeats + stripes on import. We never actually sample this image
     * (plugin only writes via TRANSFER_DST; consumer only reads via
     * TRANSFER_SRC into a separate shadow). Keeping TRANSFER_SRC in the
     * usage mask too forces both sides into the same transfer-path
     * sub-layout. */
    img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkResult r = st->vkCreateImage(st->device, &img_ci, NULL, &image);
    if (r != VK_SUCCESS) {
        fprintf(stderr,
                "ww_pool[vulkan]: vkCreateImage failed (modifier=0x%016llx linear=%d): %d\n",
                (unsigned long long)modifiers[0], linear_path ? 1 : 0, r);
        return -EIO;
    }

    VkImageMemoryRequirementsInfo2 mri = {0};
    mri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    mri.image = image;
    VkMemoryRequirements2 mr = {0};
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    st->vkGetImageMemoryRequirements2(st->device, &mri, &mr);

    uint32_t mem_type = pick_memory_type(
        st, mr.memoryRequirements.memoryTypeBits);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr,
                "ww_pool[vulkan]: no memory type matches image typeBits=0x%x\n",
                mr.memoryRequirements.memoryTypeBits);
        st->vkDestroyImage(st->device, image, NULL);
        return -EIO;
    }

    VkMemoryDedicatedAllocateInfo ded = {0};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = image;

    VkExportMemoryAllocateInfo exp = {0};
    exp.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.pNext       = &ded;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo mai = {0};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &exp;
    mai.allocationSize  = mr.memoryRequirements.size;
    mai.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = st->vkAllocateMemory(st->device, &mai, NULL, &memory);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "ww_pool[vulkan]: vkAllocateMemory failed: %d\n", r);
        st->vkDestroyImage(st->device, image, NULL);
        return -ENOMEM;
    }
    r = st->vkBindImageMemory(st->device, image, memory, 0);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "ww_pool[vulkan]: vkBindImageMemory failed: %d\n", r);
        st->vkFreeMemory(st->device, memory, NULL);
        st->vkDestroyImage(st->device, image, NULL);
        return -EIO;
    }

    VkMemoryGetFdInfoKHR fdi = {0};
    fdi.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdi.memory     = memory;
    fdi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    r = st->vkGetMemoryFdKHR(st->device, &fdi, &fd);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "ww_pool[vulkan]: vkGetMemoryFdKHR failed: %d\n", r);
        st->vkFreeMemory(st->device, memory, NULL);
        st->vkDestroyImage(st->device, image, NULL);
        return -EIO;
    }

    /* Read back the actual modifier + per-plane layouts. The directive
     * tells us how many memory planes the modifier defines; LINEAR is
     * always 1, AMD DCC w/o RETILE = 2, DCC w/ RETILE = 3, etc. */
    VkImageDrmFormatModifierPropertiesEXT mod_props = {0};
    mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    st->vkGetImageDrmFormatModifierPropertiesEXT(st->device, image, &mod_props);

    uint32_t plane_count = linear_path ? 1u : d->plane_count;
    if (plane_count == 0) plane_count = 1;
    if (plane_count > WW_POOL_MAX_PLANES) {
        fprintf(stderr,
                "ww_pool[vulkan]: alloc_slot[%u]: directive plane_count=%u "
                "exceeds WW_POOL_MAX_PLANES=%d\n",
                slot_index, plane_count, WW_POOL_MAX_PLANES);
        close(fd);
        st->vkFreeMemory(st->device, memory, NULL);
        st->vkDestroyImage(st->device, image, NULL);
        return -ENOSPC;
    }

    static const VkImageAspectFlagBits kAspects[WW_POOL_MAX_PLANES] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
    };
    uint32_t plane_strides[WW_POOL_MAX_PLANES] = {0};
    uint32_t plane_offsets[WW_POOL_MAX_PLANES] = {0};
    uint64_t plane_sizes[WW_POOL_MAX_PLANES]   = {0};
    for (uint32_t p = 0; p < plane_count; ++p) {
        VkImageSubresource s = {0};
        s.aspectMask = kAspects[p];
        VkSubresourceLayout pl = {0};
        st->vkGetImageSubresourceLayout(st->device, image, &s, &pl);
        plane_strides[p] = (uint32_t)pl.rowPitch;
        plane_offsets[p] = (uint32_t)pl.offset;
        plane_sizes[p]   = (uint64_t)pl.size;
    }

    fprintf(stderr,
            "ww_pool[vulkan]: alloc_slot[%u] %ux%u fourcc=0x%08x "
            "mod=0x%016llx linear=%d planes=%u mem_size=%llu\n",
            slot_index, d->width, d->height, d->fourcc,
            (unsigned long long)mod_props.drmFormatModifier, linear_path ? 1 : 0,
            plane_count, (unsigned long long)mr.memoryRequirements.size);
    for (uint32_t p = 0; p < plane_count; ++p) {
        fprintf(stderr,
                "  plane[%u] stride=%u offset=%u size=%llu\n",
                p, plane_strides[p], plane_offsets[p],
                (unsigned long long)plane_sizes[p]);
    }

    /* Vulkan's non-disjoint dmabuf-import gives a single VkDeviceMemory
     * with one fd; the multi-plane layout is internal to that
     * allocation. The wire protocol carries one fd per plane, so dup
     * the same fd into every plane index. The consumer receives N
     * fds that all reference the same dma-buf and applies the
     * appropriate stride/offset to each. */
    int plane_fds[WW_POOL_MAX_PLANES] = {-1, -1, -1, -1};
    plane_fds[0] = fd;
    for (uint32_t p = 1; p < plane_count; ++p) {
        plane_fds[p] = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (plane_fds[p] < 0) {
            fprintf(stderr,
                    "ww_pool[vulkan]: F_DUPFD_CLOEXEC for plane %u failed: %s\n",
                    p, strerror(errno));
            for (uint32_t q = 0; q < p; ++q) close(plane_fds[q]);
            st->vkFreeMemory(st->device, memory, NULL);
            st->vkDestroyImage(st->device, image, NULL);
            return -EIO;
        }
    }

    st->slots[slot_index].image  = image;
    st->slots[slot_index].memory = memory;

    out->plane_count = plane_count;
    for (uint32_t p = 0; p < plane_count; ++p) {
        out->fds[p]            = plane_fds[p];
        out->strides[p]        = plane_strides[p];
        out->plane_offsets[p]  = plane_offsets[p];
        out->sizes[p]          = plane_sizes[p];
    }
    for (uint32_t p = plane_count; p < WW_POOL_MAX_PLANES; ++p) {
        out->fds[p] = -1;
    }
    out->modifier = mod_props.drmFormatModifier;
    return 0;
}

static void free_slot(ww_pool_t *pool, uint32_t slot_index) {
    if (slot_index >= WW_POOL_MAX_SLOTS) return;
    vk_state_t *st = (vk_state_t *)pool->backend_data;
    /* Idle the device so we don't tear down resources still in use. */
    if (st->vkDeviceWaitIdle && st->device) {
        st->vkDeviceWaitIdle(st->device);
    }
    if (st->slots[slot_index].image != VK_NULL_HANDLE) {
        st->vkDestroyImage(st->device, st->slots[slot_index].image, NULL);
        st->slots[slot_index].image = VK_NULL_HANDLE;
    }
    if (st->slots[slot_index].memory != VK_NULL_HANDLE) {
        st->vkFreeMemory(st->device, st->slots[slot_index].memory, NULL);
        st->slots[slot_index].memory = VK_NULL_HANDLE;
    }
}

static int populate_slot_view(ww_pool_t *pool, uint32_t slot_index,
                              ww_pool_slot_t *out) {
    vk_state_t *st = (vk_state_t *)pool->backend_data;
    if (slot_index >= WW_POOL_MAX_SLOTS) return -EINVAL;
    out->vk_image           = st->slots[slot_index].image;
    out->vk_memory          = st->slots[slot_index].memory;
    out->gl_export_fbo      = 0;
    out->gl_export_texture  = 0;
    return 0;
}

static void backend_destroy(ww_pool_t *pool) {
    vk_state_t *st = (vk_state_t *)pool->backend_data;
    if (!st) return;
    free(st);
    pool->backend_data = NULL;
}

static int backend_init(ww_pool_t *pool, const void *init_data) {
    const ww_pool_vulkan_init_t *init = (const ww_pool_vulkan_init_t *)init_data;
    if (!init || !init->instance || !init->physical_device || !init->device ||
        !init->get_instance_proc_addr) {
        return -EINVAL;
    }
    vk_state_t *st = (vk_state_t *)calloc(1, sizeof(*st));
    if (!st) return -ENOMEM;

    st->instance     = (VkInstance)init->instance;
    st->phys         = (VkPhysicalDevice)init->physical_device;
    st->device       = (VkDevice)init->device;
    st->queue        = (VkQueue)init->queue;
    st->queue_family = init->queue_family_index;

    /* See pool_egl_gbm: function-pointer-vs-object-pointer trick. */
    union {
        void *(*as_obj)(void *, const char *);
        PFN_vkGetInstanceProcAddr as_fn;
    } cvt;
    cvt.as_obj = init->get_instance_proc_addr;

    int rc = load_dispatch(st, cvt.as_fn);
    if (rc != 0) {
        fprintf(stderr,
                "ww_pool[vulkan]: failed to resolve required Vulkan entry points\n");
        free(st);
        return rc;
    }

    pool->backend_data = st;
    pool->caps.drm_render_major = init->drm_render_major;
    pool->caps.drm_render_minor = init->drm_render_minor;
    if (init->device_uuid) {
        memcpy(pool->caps.device_uuid, init->device_uuid, 16);
        pool->caps.have_uuid = true;
    }
    if (init->driver_uuid) {
        memcpy(pool->caps.driver_uuid, init->driver_uuid, 16);
    }

    /* drm_fd: pool.c will create the timeline drm_syncobj on this. The
     * Vulkan path does NOT export the timeline through Vulkan — the
     * old VkProducer used VkSemaphore + OPAQUE_FD, but bridge owns
     * the syncobj directly via DRM ioctls (matches the EGL/GBM path
     * and means the daemon's reaper sees one consistent fd kind). */
    if (init->drm_render_fd >= 0) {
        /* dup so we don't double-close. */
        int dup_fd = dup(init->drm_render_fd);
        if (dup_fd < 0) {
            fprintf(stderr, "ww_pool[vulkan]: dup(drm_render_fd) failed: %s\n",
                    strerror(errno));
            free(st);
            pool->backend_data = NULL;
            return -errno;
        }
        pool->drm_fd = dup_fd;
    } else {
        int fd = ww_drm_open_first_render_node();
        if (fd < 0) {
            fprintf(stderr,
                    "ww_pool[vulkan]: no DRM render node openable for syncobj\n");
            free(st);
            pool->backend_data = NULL;
            return fd;
        }
        pool->drm_fd = fd;
    }

    return 0;
}

static const struct ww_pool_backend_ops kVulkanOps = {
    .init               = backend_init,
    .probe_caps         = probe_caps,
    .alloc_slot         = alloc_slot,
    .free_slot          = free_slot,
    .populate_slot_view = populate_slot_view,
    .destroy            = backend_destroy,
};

int ww_pool_vulkan_create(ww_pool_t *pool, const void *init_data) {
    pool->ops = &kVulkanOps;
    int rc = backend_init(pool, init_data);
    if (rc != 0) {
        pool->ops = NULL;
    }
    return rc;
}
