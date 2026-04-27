/* waywallen-bridge — Vulkan dispatch loader + GPU info logger.
 *
 * Implements probe_vk.h. No libvulkan linkage; the dispatch table is
 * populated by the plugin-supplied vkGetInstanceProcAddr against a
 * live VkInstance.
 */
#include <waywallen-bridge/probe_vk.h>
#include <waywallen-bridge/bridge.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

int ww_bridge_vk_dt_load(ww_bridge_vk_dt_t *dt,
                         PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         VkInstance instance) {
    if (!dt || !get_instance_proc_addr) return -EINVAL;
    memset(dt, 0, sizeof(*dt));

    dt->vkGetInstanceProcAddr = get_instance_proc_addr;

    /* Physical-device queries are resolved against `instance`. With
     * VK_NULL_HANDLE only the pre-instance entry points resolve, so
     * pass a real instance to populate the rest. Per-field explicit
     * casts keep the unit free of `typeof` under -Wpedantic. */
    dt->vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties");
    dt->vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties2");
    dt->vkGetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)
        get_instance_proc_addr(instance, "vkGetPhysicalDeviceFormatProperties2");
    return 0;
}

void ww_bridge_vk_log_gpu_info(const char *prefix,
                               const ww_bridge_vk_dt_t *dt,
                               VkPhysicalDevice phys) {
    if (!dt || phys == VK_NULL_HANDLE) return;
    if (!dt->vkGetPhysicalDeviceProperties && !dt->vkGetPhysicalDeviceProperties2) return;

    /* Prefer Properties2 + DriverProperties for richer driver info;
     * fall back to plain Properties when the 1.1+ chain isn't loaded. */
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));

    char drv_buf[256];
    drv_buf[0] = '\0';

    if (dt->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceDriverProperties drv;
        memset(&drv, 0, sizeof(drv));
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 p2;
        memset(&p2, 0, sizeof(p2));
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drv;

        dt->vkGetPhysicalDeviceProperties2(phys, &p2);
        props = p2.properties;

        if (drv.driverName[0] || drv.driverInfo[0]) {
            snprintf(drv_buf, sizeof(drv_buf), "%s | %s",
                     drv.driverName[0] ? drv.driverName : "(unknown)",
                     drv.driverInfo[0] ? drv.driverInfo : "(no info)");
        }
    } else {
        dt->vkGetPhysicalDeviceProperties(phys, &props);
    }

    char api_buf[32];
    snprintf(api_buf, sizeof(api_buf), "%u.%u.%u",
             VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));

    /* Render the device name through a stack copy so the helper can
     * emit a stable pointer (deviceName is a fixed-size char[]). */
    char dev_buf[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 1];
    snprintf(dev_buf, sizeof(dev_buf), "%s", props.deviceName);

    const ww_gpu_info_field_t fields[] = {
        { "device",  dev_buf },
        { "api ver", api_buf },
        { "driver",  drv_buf[0] ? drv_buf : NULL },
    };
    ww_bridge_log_gpu_info(prefix, fields,
                           sizeof(fields) / sizeof(fields[0]));
}
