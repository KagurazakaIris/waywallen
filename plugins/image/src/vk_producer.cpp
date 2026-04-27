#include "vk_producer.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

#include <waywallen-bridge/drm_fourcc.h>

namespace ww_image {

namespace {

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

const char* vk_result_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:                                return "VK_ERROR_?";
    }
}

bool device_has_ext(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, props.data());
    for (auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

bool pick_queue_family(VkPhysicalDevice phys, uint32_t* out) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, q.data());
    for (uint32_t i = 0; i < n; ++i) {
        // Transfer-capable is enough for upload. Graphics families always
        // include TRANSFER implicitly, so this picks the main queue on most
        // drivers.
        if (q[i].queueFlags
            & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT
               | VK_QUEUE_TRANSFER_BIT)) {
            *out = i;
            return true;
        }
    }
    return false;
}

} // namespace


VkProducer::~VkProducer() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (staging_map_) vkUnmapMemory(device_, staging_mem_);
        if (staging_buf_ != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, staging_buf_, nullptr);
        if (staging_mem_ != VK_NULL_HANDLE)
            vkFreeMemory(device_, staging_mem_, nullptr);
        if (signal_sem_ != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, signal_sem_, nullptr);
        if (release_timeline_sem_ != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, release_timeline_sem_, nullptr);
        if (cmd_pool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        if (image_ != VK_NULL_HANDLE)  vkDestroyImage(device_, image_, nullptr);
        if (memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, memory_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    if (layout_.dmabuf_fd >= 0) ::close(layout_.dmabuf_fd);
}


std::unique_ptr<VkProducer>
VkProducer::create(uint32_t width, uint32_t height, uint32_t flags,
                   uint64_t modifier, std::string* err) {
    if (width == 0 || height == 0) {
        fail(err, "VkProducer: width/height must be non-zero");
        return nullptr;
    }

    auto self = std::unique_ptr<VkProducer>(new VkProducer());
    self->flags_ = flags;
    self->modifier_ = modifier;
    self->layout_.width  = width;
    self->layout_.height = height;

    // --- Instance -------------------------------------------------------
    // Vulkan 1.1 promotes external memory/semaphore core structs we rely on.
    const char* inst_exts[] = {
        // Explicit enable is a no-op under 1.1 but documents intent and
        // keeps us portable to drivers reporting 1.0 + the KHR ext.
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app {};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "waywallen-image-renderer";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = static_cast<uint32_t>(std::size(inst_exts));
    ici.ppEnabledExtensionNames = inst_exts;

    if (VkResult r = vkCreateInstance(&ici, nullptr, &self->instance_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateInstance: ") + vk_result_str(r));
        return nullptr;
    }

    // --- Physical device ------------------------------------------------
    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(self->instance_, &pd_count, nullptr);
    if (pd_count == 0) {
        fail(err, "no Vulkan physical devices found");
        return nullptr;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(self->instance_, &pd_count, pds.data());

    const char* req_dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        // Required for the release timeline syncobj. Core in Vulkan 1.2;
        // we target 1.1 so explicitly enable the KHR ext.
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };
    // VK_EXT_physical_device_drm is optional. When present we use it to
    // report the renderer's DRM render-node to the daemon so it can
    // match against display GPUs. When absent we fall back to (0,0)
    // and the daemon conservatively assumes cross-GPU.
    static constexpr const char* DRM_EXT = "VK_EXT_physical_device_drm";
    for (auto pd : pds) {
        bool ok = true;
        for (const char* e : req_dev_exts) {
            if (!device_has_ext(pd, e)) { ok = false; break; }
        }
        if (ok) { self->phys_ = pd; break; }
    }
    if (self->phys_ == VK_NULL_HANDLE) {
        fail(err, "no physical device supports the DMA-BUF export extension set");
        return nullptr;
    }
    bool have_drm_ext = device_has_ext(self->phys_, DRM_EXT);

    if (!pick_queue_family(self->phys_, &self->queue_family_)) {
        fail(err, "no suitable queue family");
        return nullptr;
    }

    // --- Device ---------------------------------------------------------
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci {};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = self->queue_family_;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> dev_exts(std::begin(req_dev_exts), std::end(req_dev_exts));
    if (have_drm_ext) dev_exts.push_back(DRM_EXT);

    // Enable the timeline_semaphore device feature so we can create
    // VK_SEMAPHORE_TYPE_TIMELINE semaphores. The KHR struct alias is
    // wire-compatible with the 1.2 core struct.
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR ts_feat {};
    ts_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
    ts_feat.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo dci {};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &ts_feat;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();

    if (VkResult r = vkCreateDevice(self->phys_, &dci, nullptr, &self->device_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateDevice: ") + vk_result_str(r));
        return nullptr;
    }
    vkGetDeviceQueue(self->device_, self->queue_family_, 0, &self->queue_);

    self->vkGetMemoryFdKHR_ =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(self->device_, "vkGetMemoryFdKHR"));
    self->vkGetSemaphoreFdKHR_ =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(self->device_, "vkGetSemaphoreFdKHR"));
    self->vkGetImageDrmFormatModifierPropertiesEXT_ =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(self->device_,
                                "vkGetImageDrmFormatModifierPropertiesEXT"));
    self->vkGetPhysicalDeviceProperties2_ =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(self->instance_,
                                  "vkGetPhysicalDeviceProperties2"));
    if (!self->vkGetMemoryFdKHR_
        || !self->vkGetSemaphoreFdKHR_
        || !self->vkGetImageDrmFormatModifierPropertiesEXT_) {
        fail(err, "required device entry points missing");
        return nullptr;
    }

    // Query DRM render-node — best effort. Stays at (0,0) when the
    // extension isn't present or the device just doesn't expose a
    // render node (rare; integrated devices on some kernels).
    if (have_drm_ext && self->vkGetPhysicalDeviceProperties2_) {
        VkPhysicalDeviceDrmPropertiesEXT drm {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &drm;
        self->vkGetPhysicalDeviceProperties2_(self->phys_, &props);
        if (drm.hasRender && drm.renderMajor >= 0 && drm.renderMinor >= 0
            && (uint64_t)drm.renderMajor <= UINT32_MAX
            && (uint64_t)drm.renderMinor <= UINT32_MAX) {
            self->drm_render_major_ = static_cast<uint32_t>(drm.renderMajor);
            self->drm_render_minor_ = static_cast<uint32_t>(drm.renderMinor);
        }
    }

    // --- Image + memory + dmabuf export -------------------------------
    if (!self->build_image(err)) return nullptr;

    // --- Command pool + buffer -----------------------------------------
    VkCommandPoolCreateInfo cpi {};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = self->queue_family_;
    if (VkResult r = vkCreateCommandPool(self->device_, &cpi, nullptr,
                                         &self->cmd_pool_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateCommandPool: ") + vk_result_str(r));
        return nullptr;
    }

    VkCommandBufferAllocateInfo cbi {};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = self->cmd_pool_;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (VkResult r = vkAllocateCommandBuffers(self->device_, &cbi, &self->cmd_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkAllocateCommandBuffers: ") + vk_result_str(r));
        return nullptr;
    }

    // --- Acquire semaphore (binary, exported as SYNC_FD) ---------------
    VkExportSemaphoreCreateInfo exp_sem {};
    exp_sem.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_sem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sem_ci {};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_ci.pNext = &exp_sem;
    if (VkResult r = vkCreateSemaphore(self->device_, &sem_ci, nullptr,
                                       &self->signal_sem_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateSemaphore(acquire): ") + vk_result_str(r));
        return nullptr;
    }

    // --- Release semaphore (TIMELINE, exported as OPAQUE_FD) -----------
    // OPAQUE_FD round-trips to a drm_syncobj on Mesa drivers; the daemon
    // imports this fd and DRM_IOCTL_SYNCOBJ_TRANSFERs consumer release
    // fences onto each frame's release_point. This producer only WAITs
    // on the timeline; SIGNAL is the daemon's job.
    VkSemaphoreTypeCreateInfoKHR ts_type {};
    ts_type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    ts_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    ts_type.initialValue  = 0;

    VkExportSemaphoreCreateInfo exp_release {};
    exp_release.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_release.pNext       = &ts_type;
    exp_release.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreCreateInfo release_ci {};
    release_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    release_ci.pNext = &exp_release;
    if (VkResult r = vkCreateSemaphore(self->device_, &release_ci, nullptr,
                                       &self->release_timeline_sem_);
        r != VK_SUCCESS) {
        fail(err,
             std::string("vkCreateSemaphore(release timeline): ")
                 + vk_result_str(r));
        return nullptr;
    }

    // --- Staging buffer (HOST_VISIBLE|COHERENT, tightly packed RGBA) ---
    // For LINEAR target, Vulkan's rowPitch may exceed width*4 on drivers
    // that enforce stricter alignment. We still feed the staging buffer
    // with tightly-packed bytes (bufferRowLength=0) and let
    // vkCmdCopyBufferToImage stride the destination using the image's
    // layout. Staging size = width*height*4.
    const VkDeviceSize tight = VkDeviceSize(width) * height * 4;
    self->staging_size_ = tight;

    VkBufferCreateInfo bci {};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = tight;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VkResult r = vkCreateBuffer(self->device_, &bci, nullptr,
                                    &self->staging_buf_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateBuffer(staging): ") + vk_result_str(r));
        return nullptr;
    }

    VkMemoryRequirements bmr {};
    vkGetBufferMemoryRequirements(self->device_, self->staging_buf_, &bmr);

    VkPhysicalDeviceMemoryProperties mprops {};
    vkGetPhysicalDeviceMemoryProperties(self->phys_, &mprops);

    uint32_t host_type = UINT32_MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        const auto flags = mprops.memoryTypes[i].propertyFlags;
        if ((bmr.memoryTypeBits & (1u << i))
            && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            host_type = i;
            break;
        }
    }
    if (host_type == UINT32_MAX) {
        fail(err, "no HOST_VISIBLE|COHERENT memory type for staging");
        return nullptr;
    }

    VkMemoryAllocateInfo smai {};
    smai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    smai.allocationSize  = bmr.size;
    smai.memoryTypeIndex = host_type;
    if (VkResult r = vkAllocateMemory(self->device_, &smai, nullptr,
                                      &self->staging_mem_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkAllocateMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }
    if (VkResult r = vkBindBufferMemory(self->device_, self->staging_buf_,
                                        self->staging_mem_, 0);
        r != VK_SUCCESS) {
        fail(err, std::string("vkBindBufferMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }
    if (VkResult r = vkMapMemory(self->device_, self->staging_mem_, 0,
                                 VK_WHOLE_SIZE, 0, &self->staging_map_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkMapMemory(staging): ") + vk_result_str(r));
        return nullptr;
    }

    return self;
}


bool VkProducer::build_image(std::string* err) {
    // The image is allocated with the single DRM modifier the caller
    // requested (`modifier_` — set by `create()` / `rebuild()`).
    // Vulkan picks that modifier (it has nothing else to choose from)
    // and the actual layout for that tiling is read back below from
    // `vkGetImageDrmFormatModifierPropertiesEXT`. Note: tiling
    // (modifier) only constrains the image's data layout — it does
    // NOT pin the memory placement. The physical heap is dictated by
    // the memory-type index picked below, so cross-GPU PRIME import
    // requires WW_BUF_HOST_VISIBLE (true GTT) regardless of modifier.
    const uint64_t mods[] = { modifier_ };
    VkImageDrmFormatModifierListCreateInfoEXT mod_list {};
    mod_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    mod_list.drmFormatModifierCount = 1;
    mod_list.pDrmFormatModifiers    = mods;

    VkExternalMemoryImageCreateInfo ext_img {};
    ext_img.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.pNext       = &mod_list;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo img_ci {};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.pNext         = &ext_img;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent        = { layout_.width, layout_.height, 1 };
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = 1;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (VkResult r = vkCreateImage(device_, &img_ci, nullptr, &image_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkCreateImage: ") + vk_result_str(r));
        return false;
    }

    VkImageMemoryRequirementsInfo2 mri {};
    mri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    mri.image = image_;
    VkMemoryRequirements2 mr {};
    mr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetImageMemoryRequirements2(device_, &mri, &mr);

    VkPhysicalDeviceMemoryProperties mprops {};
    vkGetPhysicalDeviceMemoryProperties(phys_, &mprops);

    // Memory type pick: HOST_VISIBLE && !DEVICE_LOCAL when the daemon
    // asked for cross-GPU placement, plain DEVICE_LOCAL otherwise.
    // Excluding DEVICE_LOCAL on the host-visible path avoids the
    // ReBAR/SAM HOST_VISIBLE+DEVICE_LOCAL alias (which still lives in
    // VRAM and cannot be PRIME-imported by a foreign GPU).
    const bool want_host_visible = (flags_ & WW_BUF_HOST_VISIBLE) != 0;
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        if (!(mr.memoryRequirements.memoryTypeBits & (1u << i))) continue;
        const auto pf = mprops.memoryTypes[i].propertyFlags;
        const bool ok = want_host_visible
            ? ((pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                && !(pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            : (pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ok) { mem_type = i; break; }
    }
    if (mem_type == UINT32_MAX) {
        fail(err, want_host_visible
                ? "no HOST_VISIBLE non-DEVICE_LOCAL memory type for image"
                : "no DEVICE_LOCAL memory type for image");
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
        return false;
    }

    // Diagnostics: log the picked memtype + property bits so producer
    // and importer logs can be cross-referenced when cross-GPU PRIME
    // imports fail. Only the bits we actually care about are decoded.
    {
        const auto pf = mprops.memoryTypes[mem_type].propertyFlags;
        char props_buf[96] = "";
        size_t n = 0;
#define APPEND(name)                                                       \
        if ((pf & VK_MEMORY_PROPERTY_##name##_BIT)                         \
            && n < sizeof(props_buf)) {                                    \
            int w = std::snprintf(props_buf + n, sizeof(props_buf) - n,    \
                                  "%s" #name, n ? "|" : "");               \
            if (w > 0) n += static_cast<size_t>(w);                        \
        }
        APPEND(DEVICE_LOCAL);
        APPEND(HOST_VISIBLE);
        APPEND(HOST_COHERENT);
        APPEND(HOST_CACHED);
        APPEND(LAZILY_ALLOCATED);
#undef APPEND
        if (n == 0) std::snprintf(props_buf, sizeof(props_buf), "0");
        std::fprintf(stderr,
                     "waywallen-image-renderer: vk_producer build_image "
                     "flags=0x%x want_host_visible=%d picked memTypeIndex=%u "
                     "props=[%s] image.memoryTypeBits=0x%x size=%llu\n",
                     flags_, want_host_visible ? 1 : 0, mem_type, props_buf,
                     mr.memoryRequirements.memoryTypeBits,
                     static_cast<unsigned long long>(mr.memoryRequirements.size));
    }

    VkMemoryDedicatedAllocateInfo dedicated {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image_;

    VkExportMemoryAllocateInfo export_info {};
    export_info.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext       = &dedicated;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo mai {};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &export_info;
    mai.allocationSize  = mr.memoryRequirements.size;
    mai.memoryTypeIndex = mem_type;

    if (VkResult r = vkAllocateMemory(device_, &mai, nullptr, &memory_);
        r != VK_SUCCESS) {
        fail(err, std::string("vkAllocateMemory: ") + vk_result_str(r));
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
        return false;
    }
    if (VkResult r = vkBindImageMemory(device_, image_, memory_, 0);
        r != VK_SUCCESS) {
        fail(err, std::string("vkBindImageMemory: ") + vk_result_str(r));
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryGetFdInfoKHR fd_info {};
    fd_info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory     = memory_;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (VkResult r = vkGetMemoryFdKHR_(device_, &fd_info, &fd);
        r != VK_SUCCESS) {
        fail(err, std::string("vkGetMemoryFdKHR: ") + vk_result_str(r));
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
        return false;
    }

    VkImageSubresource sub {};
    sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    VkSubresourceLayout vk_layout {};
    vkGetImageSubresourceLayout(device_, image_, &sub, &vk_layout);

    VkImageDrmFormatModifierPropertiesEXT mod_props {};
    mod_props.sType =
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    vkGetImageDrmFormatModifierPropertiesEXT_(device_, image_, &mod_props);

    layout_.dmabuf_fd    = fd;
    layout_.drm_modifier = mod_props.drmFormatModifier;
    layout_.drm_fourcc   = WW_DRM_FORMAT_ABGR8888;
    layout_.plane_offset = static_cast<uint32_t>(vk_layout.offset);
    layout_.stride       = static_cast<uint32_t>(vk_layout.rowPitch);
    layout_.size         = static_cast<uint32_t>(mr.memoryRequirements.size);
    return true;
}

bool VkProducer::rebuild(uint32_t flags, uint64_t modifier, std::string* err) {
    if (device_ == VK_NULL_HANDLE) {
        return fail(err, "rebuild on uninitialised producer"), false;
    }
    vkDeviceWaitIdle(device_);
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
    if (layout_.dmabuf_fd >= 0) {
        ::close(layout_.dmabuf_fd);
        layout_.dmabuf_fd = -1;
    }
    flags_ = flags;
    modifier_ = modifier;
    return build_image(err);
}

int VkProducer::upload_and_submit(const uint8_t* data, size_t size,
                                  uint64_t wait_release_point,
                                  std::string* err) {
    if (size != staging_size_) {
        fail(err, "upload size mismatch (expected tightly-packed RGBA)");
        return -1;
    }

    std::memcpy(staging_map_, data, size);

    if (VkResult r = vkResetCommandBuffer(cmd_, 0); r != VK_SUCCESS) {
        fail(err, std::string("vkResetCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (VkResult r = vkBeginCommandBuffer(cmd_, &bi); r != VK_SUCCESS) {
        fail(err, std::string("vkBeginCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    // UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier to_dst {};
    to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask       = 0;
    to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image               = image_;
    to_dst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd_,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkBufferImageCopy region {};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0; // tightly packed
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = { 0, 0, 0 };
    region.imageExtent                     = { layout_.width, layout_.height, 1 };
    vkCmdCopyBufferToImage(cmd_, staging_buf_, image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    // TRANSFER_DST_OPTIMAL -> GENERAL, release to FOREIGN for external
    // consumer import. The consumer's driver takes ownership on import.
    VkImageMemoryBarrier to_foreign {};
    to_foreign.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_foreign.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_foreign.dstAccessMask       = 0;
    to_foreign.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_foreign.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    to_foreign.srcQueueFamilyIndex = queue_family_;
    to_foreign.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    to_foreign.image               = image_;
    to_foreign.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_foreign);

    if (VkResult r = vkEndCommandBuffer(cmd_); r != VK_SUCCESS) {
        fail(err, std::string("vkEndCommandBuffer: ") + vk_result_str(r));
        return -1;
    }

    // Gate the submit on the previous frame's release_point if there is
    // one (skip on the very first submit). Binary acquire signal value
    // is unused but the timeline submit info requires the array to be
    // the same length as pSignalSemaphores.
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const uint64_t wait_value = wait_release_point;
    const uint64_t signal_value = 0; // signal_sem_ is binary; ignored.

    VkTimelineSemaphoreSubmitInfoKHR ts_info {};
    ts_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR;
    if (wait_release_point > 0) {
        ts_info.waitSemaphoreValueCount = 1;
        ts_info.pWaitSemaphoreValues    = &wait_value;
    }
    ts_info.signalSemaphoreValueCount = 1;
    ts_info.pSignalSemaphoreValues    = &signal_value;

    VkSubmitInfo si {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = &ts_info;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &signal_sem_;
    if (wait_release_point > 0) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores    = &release_timeline_sem_;
        si.pWaitDstStageMask  = &wait_stage;
    }
    if (VkResult r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        r != VK_SUCCESS) {
        fail(err, std::string("vkQueueSubmit: ") + vk_result_str(r));
        return -1;
    }

    // Export sync_file fd. This consumes the semaphore's signal payload,
    // so the semaphore is safe to reuse in the next submit.
    VkSemaphoreGetFdInfoKHR sgfi {};
    sgfi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sgfi.semaphore  = signal_sem_;
    sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int sync_fd = -1;
    if (VkResult r = vkGetSemaphoreFdKHR_(device_, &sgfi, &sync_fd);
        r != VK_SUCCESS) {
        fail(err, std::string("vkGetSemaphoreFdKHR: ") + vk_result_str(r));
        return -1;
    }
    return sync_fd;
}

// Bit constants live in <waywallen-bridge/protocol_bits.h> (pulled
// in via vk_producer.hpp). Format-feature → usage mapping stays
// here since it's Vulkan-specific.
namespace {
uint32_t map_format_features_to_usage(VkFormatFeatureFlags ff) {
    uint32_t u = 0;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)        u |= WW_USAGE_SAMPLED;
    if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)        u |= WW_USAGE_STORAGE;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)     u |= WW_USAGE_COLOR_ATTACHMENT;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)         u |= WW_USAGE_TRANSFER_SRC;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)         u |= WW_USAGE_TRANSFER_DST;
    return u;
}
} // namespace

bool VkProducer::query_format_caps(VkFormatCaps* out, std::string* err) const {
    if (!out) {
        fail(err, "query_format_caps: out is null");
        return false;
    }
    if (phys_ == VK_NULL_HANDLE || !vkGetPhysicalDeviceProperties2_) {
        fail(err, "query_format_caps: producer not initialized");
        return false;
    }
    out->fourccs.clear();
    out->mod_counts.clear();
    out->modifiers.clear();
    out->usages.clear();
    out->plane_counts.clear();
    std::memset(out->device_uuid, 0, sizeof(out->device_uuid));
    std::memset(out->driver_uuid, 0, sizeof(out->driver_uuid));
    out->mem_hints = WW_MEM_HINT_DEVICE_LOCAL | WW_MEM_HINT_HOST_VISIBLE;

    // --- Probe modifier list for ABGR8888 (VK_FORMAT_R8G8B8A8_UNORM)
    auto vkGetPhysicalDeviceFormatProperties2_ =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
            vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFormatProperties2"));
    if (!vkGetPhysicalDeviceFormatProperties2_) {
        fail(err, "query_format_caps: vkGetPhysicalDeviceFormatProperties2 missing");
        return false;
    }

    VkDrmFormatModifierPropertiesListEXT mod_list {};
    mod_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp2 {};
    fp2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp2.pNext = &mod_list;
    // First call: count-only
    vkGetPhysicalDeviceFormatProperties2_(phys_, VK_FORMAT_R8G8B8A8_UNORM, &fp2);
    std::vector<VkDrmFormatModifierPropertiesEXT> probed(mod_list.drmFormatModifierCount);
    mod_list.pDrmFormatModifierProperties = probed.data();
    vkGetPhysicalDeviceFormatProperties2_(phys_, VK_FORMAT_R8G8B8A8_UNORM, &fp2);

    if (probed.empty()) {
        fail(err, "query_format_caps: driver reports zero modifiers for ABGR8888");
        return false;
    }

    out->fourccs.push_back(WW_DRM_FORMAT_ABGR8888);
    out->mod_counts.push_back(static_cast<uint32_t>(probed.size()));
    for (auto& m : probed) {
        out->modifiers.push_back(m.drmFormatModifier);
        out->usages.push_back(map_format_features_to_usage(m.drmFormatModifierTilingFeatures));
        out->plane_counts.push_back(m.drmFormatModifierPlaneCount);
    }

    // --- Pull device + driver UUID via VkPhysicalDeviceIDProperties (Vulkan 1.1 core)
    VkPhysicalDeviceIDProperties id_props {};
    id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 pd2 {};
    pd2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    pd2.pNext = &id_props;
    vkGetPhysicalDeviceProperties2_(phys_, &pd2);
    std::memcpy(out->device_uuid, id_props.deviceUUID, 16);
    std::memcpy(out->driver_uuid, id_props.driverUUID, 16);

    return true;
}

int VkProducer::export_release_syncobj_fd(std::string* err) {
    if (release_timeline_sem_ == VK_NULL_HANDLE || !vkGetSemaphoreFdKHR_) {
        fail(err, "release timeline semaphore not initialised");
        return -1;
    }
    VkSemaphoreGetFdInfoKHR gfi {};
    gfi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gfi.semaphore  = release_timeline_sem_;
    gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (VkResult r = vkGetSemaphoreFdKHR_(device_, &gfi, &fd);
        r != VK_SUCCESS) {
        fail(err,
             std::string("vkGetSemaphoreFdKHR(release timeline OPAQUE_FD): ")
                 + vk_result_str(r));
        return -1;
    }
    return fd;
}

} // namespace ww_image
