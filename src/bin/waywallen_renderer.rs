//! waywallen-renderer — Rust-side producer subprocess (M1 milestone).
//!
//! Honours the daemon's `ConfigureBuffers` request: at startup the pool
//! is allocated DEVICE_LOCAL (zero-copy on same-GPU paths). When the
//! daemon detects a cross-GPU consumer it asks for `BUF_HOST_VISIBLE`,
//! and we re-allocate the pool with HOST_VISIBLE && !DEVICE_LOCAL
//! memory (GTT) so the dmabuf can be PRIME-imported by another GPU.

use std::os::fd::RawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::thread;

use anyhow::{anyhow, Context, Result};
use ash::{vk, Entry, Instance};

use waywallen::ipc::proto::{ControlMsg, EventMsg};
use waywallen::ipc::uds::{recv_control, send_event};
use waywallen::renderer_manager::BUF_HOST_VISIBLE;

const SLOT_COUNT: usize = 3;
const RENDER_FORMAT: vk::Format = vk::Format::R8G8B8A8_UNORM;
const FOURCC_AB24: u32 = 0x34324241; // 'AB24', matches RGBA on the wire

struct FrameSlot {
    image: vk::Image,
    memory: vk::DeviceMemory,
}

struct PoolExport {
    fd: RawFd,
    modifier: u64,
    stride: u64,
}

struct Pool {
    slots: Vec<FrameSlot>,
    exports: Vec<PoolExport>,
    flags: u32,
    generation: u64,
}

#[derive(Debug, serde::Deserialize)]
struct Args {
    #[serde(default)]
    ipc: Option<String>,
    #[serde(default = "default_width")]
    width: u32,
    #[serde(default = "default_height")]
    height: u32,
    #[serde(default = "default_fps")]
    fps: u32,
}

fn default_width() -> u32 { 1280 }
fn default_height() -> u32 { 720 }
fn default_fps() -> u32 { 60 }

fn parse_args() -> Args {
    let mut args = Args { ipc: None, width: 1280, height: 720, fps: 60 };
    let mut iter = std::env::args().skip(1);
    while let Some(a) = iter.next() {
        match a.as_str() {
            "--ipc" => args.ipc = iter.next(),
            "--width" => args.width = iter.next().and_then(|s| s.parse().ok()).unwrap_or(1280),
            "--height" => args.height = iter.next().and_then(|s| s.parse().ok()).unwrap_or(720),
            "--fps" => args.fps = iter.next().and_then(|s| s.parse().ok()).unwrap_or(60),
            _ => { let _ = iter.next(); }
        }
    }
    args
}

fn main() -> Result<()> {
    env_logger::init();
    let args = parse_args();
    let entry = unsafe { Entry::load().context("load Vulkan")? };
    let app_info = vk::ApplicationInfo::default().api_version(vk::make_api_version(0, 1, 2, 0));
    // VK_KHR_get_physical_device_properties2 is required to query
    // VkPhysicalDeviceDrmPropertiesEXT below; on Vulkan 1.1+ it is core,
    // but we ask for the instance extension explicitly so 1.0-only
    // drivers still load (we'd then just fall back to (0,0)).
    let inst_exts = [vk::KHR_GET_PHYSICAL_DEVICE_PROPERTIES2_NAME.as_ptr()];
    let instance = unsafe {
        entry
            .create_instance(
                &vk::InstanceCreateInfo::default()
                    .application_info(&app_info)
                    .enabled_extension_names(&inst_exts),
                None,
            )
            .or_else(|_| {
                entry.create_instance(
                    &vk::InstanceCreateInfo::default().application_info(&app_info),
                    None,
                )
            })?
    };
    unsafe { libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM); }
    let result = run(&instance, &args);
    unsafe { instance.destroy_instance(None); }
    result
}

/// Read the DRM render-node major/minor of the picked physical device.
/// Requires `VK_EXT_physical_device_drm` (which the device extension
/// list must enable) — otherwise returns `(0, 0)` so the daemon
/// conservatively assumes cross-GPU.
fn query_render_node(
    instance: &Instance,
    phys: vk::PhysicalDevice,
    has_drm_ext: bool,
) -> (u32, u32) {
    if !has_drm_ext {
        return (0, 0);
    }
    let mut drm = vk::PhysicalDeviceDrmPropertiesEXT::default();
    let mut props = vk::PhysicalDeviceProperties2::default().push_next(&mut drm);
    unsafe { instance.get_physical_device_properties2(phys, &mut props); }
    if drm.has_render != vk::TRUE {
        return (0, 0);
    }
    // Vulkan reports i64 for render major/minor; clamp to u32 for the wire.
    let major = u32::try_from(drm.render_major).unwrap_or(0);
    let minor = u32::try_from(drm.render_minor).unwrap_or(0);
    (major, minor)
}

/// Pick a memory type that satisfies `req` and the placement implied by
/// `flags`. For `flags == 0` we want DEVICE_LOCAL (VRAM, zero-copy). For
/// `BUF_HOST_VISIBLE` we want HOST_VISIBLE && !DEVICE_LOCAL — true GTT,
/// not the ReBAR/SAM HOST_VISIBLE+DEVICE_LOCAL alias which still lives
/// in VRAM and can't be PRIME-imported by a foreign GPU.
fn pick_memory_type(
    mem_props: &vk::PhysicalDeviceMemoryProperties,
    type_bits: u32,
    flags: u32,
) -> Result<u32> {
    let want_host_visible = (flags & BUF_HOST_VISIBLE) != 0;
    for i in 0..mem_props.memory_type_count {
        if (type_bits & (1 << i)) == 0 {
            continue;
        }
        let f = mem_props.memory_types[i as usize].property_flags;
        let ok = if want_host_visible {
            f.contains(vk::MemoryPropertyFlags::HOST_VISIBLE)
                && !f.contains(vk::MemoryPropertyFlags::DEVICE_LOCAL)
        } else {
            f.contains(vk::MemoryPropertyFlags::DEVICE_LOCAL)
        };
        if ok {
            return Ok(i);
        }
    }
    Err(anyhow!(
        "no memory type satisfies type_bits=0x{type_bits:x} flags=0x{flags:x}"
    ))
}

/// Allocate `SLOT_COUNT` images, bind memory chosen per `flags`, export
/// each as a dma-buf fd, and return both the live Vulkan handles (so
/// the caller can free them later) and the export records (fd +
/// modifier + stride).
fn allocate_pool(
    instance: &Instance,
    device: &ash::Device,
    phys: vk::PhysicalDevice,
    width: u32,
    height: u32,
    flags: u32,
) -> Result<(Vec<FrameSlot>, Vec<PoolExport>)> {
    let mem_props = unsafe { instance.get_physical_device_memory_properties(phys) };
    let ext_mem_fd = ash::khr::external_memory_fd::Device::new(instance, device);
    let drm_mod = ash::ext::image_drm_format_modifier::Device::new(instance, device);

    let mut slots = Vec::with_capacity(SLOT_COUNT);
    let mut exports = Vec::with_capacity(SLOT_COUNT);
    for _ in 0..SLOT_COUNT {
        let mut mod_list =
            vk::ImageDrmFormatModifierListCreateInfoEXT::default().drm_format_modifiers(&[0]);
        let mut ext_info = vk::ExternalMemoryImageCreateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let img = unsafe {
            device.create_image(
                &vk::ImageCreateInfo::default()
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(RENDER_FORMAT)
                    .extent(vk::Extent3D { width, height, depth: 1 })
                    .mip_levels(1)
                    .array_layers(1)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
                    .usage(vk::ImageUsageFlags::TRANSFER_DST)
                    .push_next(&mut mod_list)
                    .push_next(&mut ext_info),
                None,
            )?
        };
        let req = unsafe { device.get_image_memory_requirements(img) };
        let mtype = pick_memory_type(&mem_props, req.memory_type_bits, flags)?;
        let mut exp = vk::ExportMemoryAllocateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let mem = unsafe {
            device.allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(req.size)
                    .memory_type_index(mtype)
                    .push_next(&mut exp),
                None,
            )?
        };
        unsafe { device.bind_image_memory(img, mem, 0)?; }

        let fd = unsafe {
            ext_mem_fd.get_memory_fd(
                &vk::MemoryGetFdInfoKHR::default()
                    .memory(mem)
                    .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT),
            )?
        };
        let mut props = vk::ImageDrmFormatModifierPropertiesEXT::default();
        unsafe {
            drm_mod.get_image_drm_format_modifier_properties(img, &mut props)?;
        }
        let layout = unsafe {
            device.get_image_subresource_layout(
                img,
                vk::ImageSubresource {
                    aspect_mask: vk::ImageAspectFlags::MEMORY_PLANE_0_EXT,
                    mip_level: 0,
                    array_layer: 0,
                },
            )
        };
        slots.push(FrameSlot { image: img, memory: mem });
        exports.push(PoolExport {
            fd,
            modifier: props.drm_format_modifier,
            stride: layout.row_pitch,
        });
    }
    log::info!(
        "allocated pool: {SLOT_COUNT} slots, {width}x{height}, flags=0x{flags:x}"
    );
    Ok((slots, exports))
}

fn destroy_pool(device: &ash::Device, slots: Vec<FrameSlot>, exports: Vec<PoolExport>) {
    unsafe {
        for s in slots {
            device.destroy_image(s.image, None);
            device.free_memory(s.memory, None);
        }
    }
    // The kernel keeps the dma-buf alive for whoever holds a reference
    // (the daemon dup'd ours into its sendmsg). We close our local
    // copies now to avoid leaking fds across rebinds.
    for e in exports {
        unsafe { libc::close(e.fd); }
    }
}

/// Transition every image in the pool to GENERAL so subsequent
/// cmd_clear_color_image calls don't trip layout asserts.
fn transition_pool_to_general(
    device: &ash::Device,
    queue: vk::Queue,
    cmd_buf: vk::CommandBuffer,
    slots: &[FrameSlot],
) -> Result<()> {
    unsafe {
        device.begin_command_buffer(
            cmd_buf,
            &vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
        )?;
        for s in slots {
            let b = vk::ImageMemoryBarrier::default()
                .old_layout(vk::ImageLayout::UNDEFINED)
                .new_layout(vk::ImageLayout::GENERAL)
                .image(s.image)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .level_count(1)
                        .layer_count(1),
                );
            device.cmd_pipeline_barrier(
                cmd_buf,
                vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[b],
            );
        }
        device.end_command_buffer(cmd_buf)?;
        device.queue_submit(
            queue,
            &[vk::SubmitInfo::default().command_buffers(&[cmd_buf])],
            vk::Fence::null(),
        )?;
        device.queue_wait_idle(queue)?;
    }
    Ok(())
}

fn send_bind_buffers(
    stream: &UnixStream,
    pool: &Pool,
    width: u32,
    height: u32,
) -> Result<()> {
    let fds: Vec<RawFd> = pool.exports.iter().map(|e| e.fd).collect();
    let bind = EventMsg::BindBuffers {
        generation: pool.generation,
        flags: pool.flags,
        count: SLOT_COUNT as u32,
        fourcc: FOURCC_AB24,
        width,
        height,
        stride: pool.exports[0].stride as u32,
        modifier: pool.exports[0].modifier,
        plane_offset: 0,
        sizes: vec![pool.exports[0].stride * height as u64; SLOT_COUNT],
    };
    send_event(stream, &bind, &fds).map_err(|e| anyhow!("send BindBuffers: {e}"))?;
    log::info!(
        "sent BindBuffers: gen={}, flags=0x{:x}, modifier=0x{:x}",
        pool.generation,
        pool.flags,
        pool.exports[0].modifier
    );
    Ok(())
}

fn run(instance: &Instance, args: &Args) -> Result<()> {
    let phys = unsafe { instance.enumerate_physical_devices()?[0] };
    let families = unsafe { instance.get_physical_device_queue_family_properties(phys) };
    let gfx_family = families
        .iter()
        .enumerate()
        .find(|(_, f)| f.queue_flags.contains(vk::QueueFlags::GRAPHICS))
        .map(|(i, _)| i as u32)
        .ok_or_else(|| anyhow!("no gfx"))?;

    // Probe extensions on the chosen physical device. VK_EXT_physical_device_drm
    // is a device-level extension that's queryable via the property2 path
    // even before vkCreateDevice; we must still enable it on the device
    // for the property struct to be filled in.
    let avail_dev_exts = unsafe {
        instance
            .enumerate_device_extension_properties(phys)
            .unwrap_or_default()
    };
    let drm_ext_avail = avail_dev_exts.iter().any(|p| {
        let c = unsafe { std::ffi::CStr::from_ptr(p.extension_name.as_ptr()) };
        c.to_bytes() == b"VK_EXT_physical_device_drm"
    });

    let mut ext_names: Vec<*const i8> = vec![
        vk::KHR_EXTERNAL_MEMORY_NAME.as_ptr(),
        vk::KHR_EXTERNAL_MEMORY_FD_NAME.as_ptr(),
        vk::EXT_EXTERNAL_MEMORY_DMA_BUF_NAME.as_ptr(),
        vk::EXT_IMAGE_DRM_FORMAT_MODIFIER_NAME.as_ptr(),
        vk::KHR_EXTERNAL_SEMAPHORE_NAME.as_ptr(),
        vk::KHR_EXTERNAL_SEMAPHORE_FD_NAME.as_ptr(),
    ];
    if drm_ext_avail {
        ext_names.push(c"VK_EXT_physical_device_drm".as_ptr());
    } else {
        log::warn!(
            "VK_EXT_physical_device_drm unavailable; reporting (0,0) — \
             daemon will force HOST_VISIBLE for every consumer"
        );
    }
    let device = unsafe {
        instance.create_device(
            phys,
            &vk::DeviceCreateInfo::default()
                .queue_create_infos(&[vk::DeviceQueueCreateInfo::default()
                    .queue_family_index(gfx_family)
                    .queue_priorities(&[1.0])])
                .enabled_extension_names(&ext_names),
            None,
        )?
    };

    let (drm_major, drm_minor) = query_render_node(instance, phys, drm_ext_avail);

    let ipc_path = args.ipc.as_ref().ok_or_else(|| anyhow!("--ipc required"))?;
    let stream = UnixStream::connect(ipc_path)?;
    send_event(
        &stream,
        &EventMsg::Ready {
            drm_render_major: drm_major,
            drm_render_minor: drm_minor,
        },
        &[],
    )
    .map_err(|e| anyhow!("send Ready: {e}"))?;
    log::info!("Ready sent: drm_render={drm_major}:{drm_minor}");

    // Initial pool: zero-copy DEVICE_LOCAL. The daemon will follow up
    // with ConfigureBuffers if any consumer is on a different GPU.
    let (slots, exports) = allocate_pool(instance, &device, phys, args.width, args.height, 0)?;
    let pool = Arc::new(StdMutex::new(Pool {
        slots,
        exports,
        flags: 0,
        generation: 1,
    }));
    {
        let p = pool.lock().unwrap();
        send_bind_buffers(&stream, &p, args.width, args.height)?;
    }

    // Reader thread: forwards Shutdown via `shutdown` flag and stages
    // ConfigureBuffers via `pending_reconfig` for the main loop to pick
    // up between frames. We can't free Vulkan resources from this
    // thread because the device has to be idle and only the main
    // thread holds the queue.
    let shutdown = Arc::new(AtomicBool::new(false));
    let pending_reconfig: Arc<StdMutex<Option<u32>>> = Arc::new(StdMutex::new(None));
    let s2 = shutdown.clone();
    let p2 = pending_reconfig.clone();
    let rs = stream.try_clone()?;
    thread::spawn(move || loop {
        match recv_control(&rs) {
            Ok((ControlMsg::Shutdown, _)) => {
                s2.store(true, Ordering::SeqCst);
                return;
            }
            Ok((ControlMsg::NegotiateBuffers { mem_hint, .. }, _)) => {
                // Modifier-negotiation v2 — map mem_hint → BUF_HOST_VISIBLE
                // (bit 0). The Rust test renderer doesn't yet honor
                // fourcc/modifier overrides; it always allocates
                // ABGR8888 + LINEAR. The picker collapses to that for
                // prototype peer combos so the simplification is safe.
                const BUF_HOST_VISIBLE: u32 = 1 << 0;
                const MEM_HINT_HOST_VISIBLE: u32 = 1 << 1;
                let flags =
                    if mem_hint & MEM_HINT_HOST_VISIBLE != 0 { BUF_HOST_VISIBLE } else { 0 };
                if let Ok(mut g) = p2.lock() {
                    *g = Some(flags);
                }
            }
            Ok(_) => {}
            Err(_) => return,
        }
    });

    let queue = unsafe { device.get_device_queue(gfx_family, 0) };
    let cmd_pool = unsafe {
        device.create_command_pool(
            &vk::CommandPoolCreateInfo::default()
                .queue_family_index(gfx_family)
                .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
            None,
        )?
    };
    let cmd_buf = unsafe {
        device.allocate_command_buffers(
            &vk::CommandBufferAllocateInfo::default()
                .command_pool(cmd_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1),
        )?[0]
    };

    {
        let p = pool.lock().unwrap();
        transition_pool_to_general(&device, queue, cmd_buf, &p.slots)?;
    }

    // Per-frame sync_fd export: one exportable semaphore, reused across
    // frames. vkGetSemaphoreFdKHR with SYNC_FD handle type consumes the
    // signaled state and leaves the semaphore unsignaled for the next
    // submit (VK spec §7.4.3 "Importing Semaphore Payloads" note on
    // permanence). The exported fd is a dma_fence sync_file that the
    // display side can wait on via VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD
    // or EGL_ANDROID_native_fence_sync.
    let ext_sem_fd = ash::khr::external_semaphore_fd::Device::new(instance, &device);
    let mut export_sem_info = vk::ExportSemaphoreCreateInfo::default()
        .handle_types(vk::ExternalSemaphoreHandleTypeFlags::SYNC_FD);
    let signal_sem = unsafe {
        device.create_semaphore(
            &vk::SemaphoreCreateInfo::default().push_next(&mut export_sem_info),
            None,
        )?
    };

    let frame_period = std::time::Duration::from_secs_f64(1.0 / args.fps as f64);
    let start = std::time::Instant::now();
    let mut seq: u64 = 0;
    while !shutdown.load(Ordering::SeqCst) {
        // Honour any pending reconfigure before kicking the next frame.
        let want_flags = pending_reconfig.lock().ok().and_then(|mut g| g.take());
        if let Some(new_flags) = want_flags {
            let current_flags = pool.lock().unwrap().flags;
            if new_flags != current_flags {
                log::info!(
                    "ConfigureBuffers: rebuilding pool flags 0x{current_flags:x} → 0x{new_flags:x}"
                );
                unsafe { device.device_wait_idle()?; }
                let (new_slots, new_exports) =
                    allocate_pool(instance, &device, phys, args.width, args.height, new_flags)?;
                let next_gen = {
                    let mut p = pool.lock().unwrap();
                    let old_slots = std::mem::take(&mut p.slots);
                    let old_exports = std::mem::take(&mut p.exports);
                    destroy_pool(&device, old_slots, old_exports);
                    p.slots = new_slots;
                    p.exports = new_exports;
                    p.flags = new_flags;
                    p.generation += 1;
                    p.generation
                };
                {
                    let p = pool.lock().unwrap();
                    transition_pool_to_general(&device, queue, cmd_buf, &p.slots)?;
                    let _ = next_gen; // already stamped into pool.generation
                    send_bind_buffers(&stream, &p, args.width, args.height)?;
                }
            }
        }

        let slot = (seq as usize) % SLOT_COUNT;
        let r = (seq as f32 * 0.1).sin() * 0.5 + 0.5;
        let slot_image = pool.lock().unwrap().slots[slot].image;
        unsafe {
            device.begin_command_buffer(
                cmd_buf,
                &vk::CommandBufferBeginInfo::default()
                    .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
            )?;
            device.cmd_clear_color_image(
                cmd_buf,
                slot_image,
                vk::ImageLayout::GENERAL,
                &vk::ClearColorValue { float32: [r, 0.5, 0.5, 1.0] },
                &[vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .level_count(1)
                    .layer_count(1)],
            );
            device.end_command_buffer(cmd_buf)?;
            let signal_sems = [signal_sem];
            device.queue_submit(
                queue,
                &[vk::SubmitInfo::default()
                    .command_buffers(&[cmd_buf])
                    .signal_semaphores(&signal_sems)],
                vk::Fence::null(),
            )?;
        }
        // Export the signaled semaphore as a dma_fence sync_file fd.
        // This consumes the semaphore's signaled state; after this call
        // the semaphore is unsignaled and can be signaled again by the
        // next queue_submit. The returned fd is transferred to the
        // sendmsg cmsg immediately below.
        let sync_fd = unsafe {
            ext_sem_fd.get_semaphore_fd(
                &vk::SemaphoreGetFdInfoKHR::default()
                    .semaphore(signal_sem)
                    .handle_type(vk::ExternalSemaphoreHandleTypeFlags::SYNC_FD),
            )?
        };
        let ts_ns = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(1);
        let send_result = send_event(
            &stream,
            &EventMsg::FrameReady {
                image_index: slot as u32,
                seq,
                ts_ns,
                // TODO(release-syncobj): once this demo grows a real
                // release timeline, advance a per-slot point here and
                // emit the ReleaseSyncobj export event at startup.
                release_point: 0,
            },
            &[sync_fd],
        );
        // SCM_RIGHTS dup'd the fd into the kernel's message buffer on
        // success. Close our local copy either way: on success the
        // receiver has its own copy, on failure it's just a leak.
        unsafe { libc::close(sync_fd); }
        let _ = send_result;
        seq += 1;
        let next = start + frame_period * seq as u32;
        let now = std::time::Instant::now();
        if next > now { thread::sleep(next - now); }
    }

    unsafe {
        device.device_wait_idle()?;
        device.destroy_semaphore(signal_sem, None);
        let mut p = pool.lock().unwrap();
        let slots = std::mem::take(&mut p.slots);
        let exports = std::mem::take(&mut p.exports);
        destroy_pool(&device, slots, exports);
        device.destroy_device(None);
    }
    Ok(())
}
