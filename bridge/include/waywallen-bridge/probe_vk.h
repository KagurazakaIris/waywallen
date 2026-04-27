/* waywallen-bridge — Vulkan dispatch table and probe helpers.
 *
 * Bridge does NOT link libvulkan. Function-pointer types are pulled
 * from <vulkan/vulkan.h>; the plugin loads `vkGetInstanceProcAddr`
 * (typically via dlsym from libvulkan, or by chaining through
 * `vkCreateInstance`'s pNext) and hands it to bridge to populate the
 * dispatch table.
 *
 * Pattern mirrors `probe_egl.h`. Plugins pick exactly one backend at
 * init time.
 */
#ifndef WAYWALLEN_BRIDGE_PROBE_VK_H
#define WAYWALLEN_BRIDGE_PROBE_VK_H

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch table populated by ww_bridge_vk_dt_load. NULL members
 * mean the entry point wasn't resolvable on the supplied
 * instance/device; helpers check before invoking. */
typedef struct ww_bridge_vk_dt {
    PFN_vkGetInstanceProcAddr                  vkGetInstanceProcAddr;
    /* Physical-device queries. Resolved against the live instance. */
    PFN_vkGetPhysicalDeviceProperties          vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2         vkGetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFormatProperties2   vkGetPhysicalDeviceFormatProperties2;
} ww_bridge_vk_dt_t;

/* Resolve every dispatch entry by calling `get_instance_proc_addr`
 * with `instance`. Pre-instance entry points (the ones legal with
 * `instance == VK_NULL_HANDLE`) are not part of this table — Vulkan
 * lets you call them on the loader-provided gIPA before
 * vkCreateInstance, and bridge has no use for them.
 *
 * Returns 0 on success, -EINVAL if `dt` or `get_instance_proc_addr`
 * is NULL. Members stay NULL where the entry point isn't
 * resolvable. */
int ww_bridge_vk_dt_load(ww_bridge_vk_dt_t *dt,
                         PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance);

/* Print a "GPU info" diagnostic block to stderr for the picked
 * physical device:
 *
 *     {prefix}: GPU info
 *       device:    {deviceName}
 *       api ver:   {major.minor.patch}
 *       driver:    {driverName} | {driverInfo}     (when DriverProperties available)
 *
 * Uses `vkGetPhysicalDeviceProperties2` + VkPhysicalDeviceDriverProperties
 * when available; falls back to `vkGetPhysicalDeviceProperties` for
 * the bare-minimum device name + api version. No-op when both are
 * NULL or `phys` is VK_NULL_HANDLE. */
void ww_bridge_vk_log_gpu_info(const char *prefix,
                               const ww_bridge_vk_dt_t *dt,
                               VkPhysicalDevice phys);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_PROBE_VK_H */
