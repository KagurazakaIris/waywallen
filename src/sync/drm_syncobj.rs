//! Thin wrapper around the kernel `DRM_IOCTL_SYNCOBJ_*` API.
//!
//! Used by the daemon to allocate per-consumer release syncobjs, import
//! the producer's exported timeline syncobj, and (eventually) transfer
//! consumer release fences onto producer timeline points.
//!
//! Only the subset the daemon needs is exposed here. Vulkan-side use
//! (semaphore import/export) is not the daemon's concern — it's the
//! producer/consumer subprocesses that touch Vulkan.
//!
//! Numeric ioctl base + sequence numbers come from `<linux/drm.h>`.
//! `DRM_IOCTL_BASE` = `'d'`. Sequence numbers chosen here cover only
//! what the daemon needs today; add more when the reaper grows
//! `TIMELINE_WAIT` / `TIMELINE_SIGNAL` / `TRANSFER`.

use nix::ioctl_readwrite;
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::PathBuf;

const DRM_IOCTL_BASE: u8 = b'd';

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjCreate {
    pub handle: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjDestroy {
    pub handle: u32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjHandle {
    pub handle: u32,
    pub flags: u32,
    pub fd: i32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjTransfer {
    pub src_handle: u32,
    pub dst_handle: u32,
    pub src_point: u64,
    pub dst_point: u64,
    pub flags: u32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjWait {
    pub handles: u64,        // userspace ptr to u32[count_handles]
    pub timeout_nsec: i64,   // absolute CLOCK_MONOTONIC nsec; i64::MAX = forever
    pub count_handles: u32,
    pub flags: u32,
    pub first_signaled: u32,
    pub pad: u32,
}

pub const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL: u32 = 1 << 0;
pub const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT: u32 = 1 << 1;

#[repr(C)]
#[derive(Default, Copy, Clone, Debug)]
pub struct DrmSyncobjArray {
    pub handles: u64,        // userspace ptr to u32[count_handles]
    pub count_handles: u32,
    pub pad: u32,
}

ioctl_readwrite!(drm_syncobj_create_ioctl, DRM_IOCTL_BASE, 0xBF, DrmSyncobjCreate);
ioctl_readwrite!(drm_syncobj_destroy_ioctl, DRM_IOCTL_BASE, 0xC0, DrmSyncobjDestroy);
ioctl_readwrite!(drm_syncobj_handle_to_fd_ioctl, DRM_IOCTL_BASE, 0xC1, DrmSyncobjHandle);
ioctl_readwrite!(drm_syncobj_fd_to_handle_ioctl, DRM_IOCTL_BASE, 0xC2, DrmSyncobjHandle);
ioctl_readwrite!(drm_syncobj_wait_ioctl, DRM_IOCTL_BASE, 0xC3, DrmSyncobjWait);
ioctl_readwrite!(drm_syncobj_signal_ioctl, DRM_IOCTL_BASE, 0xC5, DrmSyncobjArray);
ioctl_readwrite!(drm_syncobj_transfer_ioctl, DRM_IOCTL_BASE, 0xCC, DrmSyncobjTransfer);

// EXPORT_SYNC_FILE and IMPORT_SYNC_FILE are NOT separate ioctls — they
// are HANDLE_TO_FD / FD_TO_HANDLE invoked with this flag bit set. The
// kernel returns EACCES (or just plain rejects) anything else.
pub const DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE: u32 = 1 << 0;
pub const DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE: u32 = 1 << 0;

// `<linux/sync_file.h>` SYNC_IOC_MERGE — merges two dma_fence sync_file
// fds into a fresh sync_file fd whose fence becomes signaled iff both
// inputs are signaled. Operates on the sync_file fd directly (not on
// any DRM device); ioctl base is `'>'`, seq 3.
const SYNC_IOC_MAGIC: u8 = b'>';
#[repr(C)]
pub struct SyncMergeData {
    pub name: [u8; 32],
    pub fd2: i32,
    pub fence: i32, // OUT: merged fd
    pub flags: u32,
    pub pad: u32,
}
ioctl_readwrite!(sync_ioc_merge, SYNC_IOC_MAGIC, 3, SyncMergeData);

fn errno_to_io(e: nix::errno::Errno) -> io::Error {
    io::Error::from_raw_os_error(e as i32)
}

/// Open render node owning `fd`. The daemon needs this for every
/// drm_syncobj ioctl. Today we open the first usable `/dev/dri/renderD*`
/// because the daemon's per-consumer release syncobjs do not have to
/// live on the same render node as the producer — drm_syncobj fds are
/// portable across DRM devices that share the dma-fence infrastructure
/// (i.e. all in-tree drivers). When the reaper learns to transfer onto
/// the producer's timeline syncobj it will need to be smarter and use
/// the producer-reported render node.
pub struct DrmDevice {
    fd: OwnedFd,
    path: PathBuf,
}

impl DrmDevice {
    pub fn open_first_render_node() -> io::Result<Self> {
        for minor in 128..=192 {
            let path = PathBuf::from(format!("/dev/dri/renderD{minor}"));
            if !path.exists() {
                continue;
            }
            match std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&path)
            {
                Ok(file) => {
                    use std::os::fd::IntoRawFd;
                    let raw = file.into_raw_fd();
                    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
                    return Ok(Self { fd, path });
                }
                Err(e) if e.kind() == io::ErrorKind::PermissionDenied => continue,
                Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
                Err(e) => return Err(e),
            }
        }
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            "no /dev/dri/renderD* could be opened (rw)",
        ))
    }

    pub fn path(&self) -> &std::path::Path {
        &self.path
    }

    pub fn as_raw_fd(&self) -> RawFd {
        self.fd.as_raw_fd()
    }

    /// Allocate a fresh binary drm_syncobj. The returned handle owns the
    /// kernel object lifetime; drop it to issue `DRM_IOCTL_SYNCOBJ_DESTROY`.
    /// Exported fds (via [`Self::handle_to_fd`]) hold their own refcount,
    /// so it is safe to drop the handle after exporting an fd that
    /// will be sent to a consumer.
    pub fn create_binary_syncobj(&self) -> io::Result<SyncobjHandle> {
        let mut arg = DrmSyncobjCreate { handle: 0, flags: 0 };
        unsafe {
            drm_syncobj_create_ioctl(self.fd.as_raw_fd(), &mut arg).map_err(errno_to_io)?;
        }
        Ok(SyncobjHandle {
            device_fd: self.fd.as_raw_fd(),
            handle: arg.handle,
        })
    }

    /// Export an fd referring to `handle`. Caller owns the returned fd.
    pub fn handle_to_fd(&self, handle: &SyncobjHandle) -> io::Result<OwnedFd> {
        let mut arg = DrmSyncobjHandle {
            handle: handle.handle,
            flags: 0,
            fd: -1,
            pad: 0,
        };
        unsafe {
            drm_syncobj_handle_to_fd_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        if arg.fd < 0 {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD returned invalid fd",
            ));
        }
        Ok(unsafe { OwnedFd::from_raw_fd(arg.fd) })
    }

    /// Import an external syncobj fd (e.g. one a producer exported via
    /// `vkGetSemaphoreFdKHR(OPAQUE_FD)` from a TIMELINE semaphore) into
    /// a fresh handle on this device. The handle is owned by the
    /// returned [`SyncobjHandle`]; dropping it issues
    /// `DRM_IOCTL_SYNCOBJ_DESTROY` (which only releases this device's
    /// reference — the kernel object lives as long as any other fd or
    /// handle still refers to it). The original `fd` is unaffected.
    pub fn fd_to_handle(&self, fd: &OwnedFd) -> io::Result<SyncobjHandle> {
        let mut arg = DrmSyncobjHandle {
            handle: 0,
            flags: 0,
            fd: fd.as_raw_fd(),
            pad: 0,
        };
        unsafe {
            drm_syncobj_fd_to_handle_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        if arg.handle == 0 {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE returned handle 0",
            ));
        }
        Ok(SyncobjHandle {
            device_fd: self.fd.as_raw_fd(),
            handle: arg.handle,
        })
    }

    /// Block until every handle in `handles` is signaled.
    /// `timeout_nsec` is an absolute `CLOCK_MONOTONIC` nanosecond
    /// deadline; pass `i64::MAX` to wait forever. Returns `Ok(())` on
    /// all-signaled, `Err(io::Error)` on timeout (errno `ETIME`) or
    /// other ioctl failure.
    ///
    /// Blocks the calling thread — call from `tokio::task::spawn_blocking`
    /// in async contexts.
    pub fn wait_handles_signaled(
        &self,
        handles: &[&SyncobjHandle],
        timeout_nsec: i64,
    ) -> io::Result<()> {
        if handles.is_empty() {
            return Ok(());
        }
        let mut raw: Vec<u32> = handles.iter().map(|h| h.handle).collect();
        // WAIT_FOR_SUBMIT is mandatory here: the reaper waits on freshly
        // created consumer binary syncobjs whose kernel fence is NULL until
        // the consumer's GPU work runs and IMPORT_SYNC_FILEs a fence in.
        // Without this flag the kernel rejects a NULL-fence wait with
        // EINVAL immediately instead of blocking until the fence appears
        // (or until our absolute CLOCK_MONOTONIC `timeout_nsec` lapses).
        let mut arg = DrmSyncobjWait {
            handles: raw.as_mut_ptr() as u64,
            timeout_nsec,
            count_handles: raw.len() as u32,
            flags: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
                | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
            first_signaled: 0,
            pad: 0,
        };
        unsafe {
            drm_syncobj_wait_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        Ok(())
    }

    /// Single-handle convenience over [`Self::wait_handles_signaled`].
    pub fn wait_signaled(&self, handle: &SyncobjHandle, timeout_nsec: i64) -> io::Result<()> {
        self.wait_handles_signaled(&[handle], timeout_nsec)
    }

    /// Export the dma_fence currently held by `handle` (binary syncobj
    /// must be signaled or have a pending fence) as a sync_file fd.
    /// The fd can be `SYNC_IOC_MERGE`d with other sync_file fds and
    /// then re-imported into a syncobj via [`Self::import_sync_file`].
    ///
    /// Implementation note: the kernel does NOT have a separate
    /// `EXPORT_SYNC_FILE` ioctl — it overloads `HANDLE_TO_FD` with the
    /// `EXPORT_SYNC_FILE` flag, returning a sync_file fd instead of a
    /// drm_syncobj fd.
    pub fn export_sync_file(&self, handle: &SyncobjHandle) -> io::Result<OwnedFd> {
        let mut arg = DrmSyncobjHandle {
            handle: handle.handle,
            flags: DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
            fd: -1,
            pad: 0,
        };
        unsafe {
            drm_syncobj_handle_to_fd_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        if arg.fd < 0 {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "HANDLE_TO_FD(EXPORT_SYNC_FILE) returned invalid fd",
            ));
        }
        Ok(unsafe { OwnedFd::from_raw_fd(arg.fd) })
    }

    /// Import a sync_file fd into `handle`, replacing whatever fence
    /// the syncobj was holding. Inverse of [`Self::export_sync_file`].
    /// `sync_file` ownership stays with the caller (the kernel dup's
    /// internally).
    ///
    /// Implementation note: piggybacks on `FD_TO_HANDLE` with the
    /// `IMPORT_SYNC_FILE` flag (see [`Self::export_sync_file`]). The
    /// `handle` field is *input* in this mode (which existing handle
    /// to write into) rather than the usual output.
    pub fn import_sync_file(&self, handle: &SyncobjHandle, sync_file: &OwnedFd) -> io::Result<()> {
        let mut arg = DrmSyncobjHandle {
            handle: handle.handle,
            flags: DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
            fd: sync_file.as_raw_fd(),
            pad: 0,
        };
        unsafe {
            drm_syncobj_fd_to_handle_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        Ok(())
    }

    /// Mark `handle` as signaled (CPU-side, non-blocking). For binary
    /// syncobjs only — calling on a TIMELINE handle errors with EINVAL.
    /// Used by consumers that don't own a real GPU release fence to
    /// unblock the daemon's reaper TRANSFER.
    pub fn signal(&self, handle: &SyncobjHandle) -> io::Result<()> {
        let mut handles = [handle.handle];
        let mut arg = DrmSyncobjArray {
            handles: handles.as_mut_ptr() as u64,
            count_handles: 1,
            pad: 0,
        };
        unsafe {
            drm_syncobj_signal_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        Ok(())
    }

    /// Transfer the signaled state at `src_point` on `src` to `dst_point`
    /// on `dst`. For a binary syncobj `src_point` is always 0; for a
    /// timeline `dst` write the post-merge fence at the desired point.
    pub fn transfer(
        &self,
        src: &SyncobjHandle,
        src_point: u64,
        dst: &SyncobjHandle,
        dst_point: u64,
    ) -> io::Result<()> {
        let mut arg = DrmSyncobjTransfer {
            src_handle: src.handle,
            dst_handle: dst.handle,
            src_point,
            dst_point,
            flags: 0,
            pad: 0,
        };
        unsafe {
            drm_syncobj_transfer_ioctl(self.fd.as_raw_fd(), &mut arg)
                .map_err(errno_to_io)?;
        }
        Ok(())
    }
}

/// Merge two sync_file fds into a fresh sync_file whose fence is the
/// AND of the two inputs (signaled iff both are signaled). Used by
/// the reaper to combine multiple consumers' release fences into the
/// single fence it `IMPORT_SYNC_FILE`s back into a binary syncobj for
/// `TRANSFER`-onto-timeline.
///
/// Caller keeps ownership of `a` and `b`; the kernel dup's their
/// fences internally. Returns the new sync_file fd.
pub fn merge_sync_files(a: &OwnedFd, b: &OwnedFd) -> io::Result<OwnedFd> {
    let mut arg = SyncMergeData {
        name: *b"waywallen-reaper-merge          ",
        fd2: b.as_raw_fd(),
        fence: -1,
        flags: 0,
        pad: 0,
    };
    // SYNC_IOC_MERGE is invoked on `a`; `b` rides along in the struct.
    unsafe {
        sync_ioc_merge(a.as_raw_fd(), &mut arg).map_err(errno_to_io)?;
    }
    if arg.fence < 0 {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            "SYNC_IOC_MERGE returned invalid fd",
        ));
    }
    Ok(unsafe { OwnedFd::from_raw_fd(arg.fence) })
}

/// RAII wrapper around a drm_syncobj kernel handle. Drop calls
/// `DRM_IOCTL_SYNCOBJ_DESTROY` on the device the handle was allocated
/// from.
pub struct SyncobjHandle {
    device_fd: RawFd,
    handle: u32,
}

impl SyncobjHandle {
    pub fn raw(&self) -> u32 {
        self.handle
    }
}

impl Drop for SyncobjHandle {
    fn drop(&mut self) {
        let mut arg = DrmSyncobjDestroy {
            handle: self.handle,
            pad: 0,
        };
        // Best-effort cleanup; nothing actionable on failure.
        unsafe {
            let _ = drm_syncobj_destroy_ioctl(self.device_fd, &mut arg);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn drm_available() -> bool {
        // Skip on hosts that don't expose any render node (CI sandboxes,
        // headless build boxes). Open is the cheapest probe.
        DrmDevice::open_first_render_node().is_ok()
    }

    #[test]
    fn open_render_node_smoke() {
        if !drm_available() {
            eprintln!("skip: no /dev/dri/renderD* available");
            return;
        }
        let dev = DrmDevice::open_first_render_node().unwrap();
        assert!(dev.as_raw_fd() >= 0);
        assert!(dev.path().to_string_lossy().contains("/dev/dri/render"));
    }

    #[test]
    fn create_export_drop_roundtrip() {
        if !drm_available() {
            eprintln!("skip: no /dev/dri/renderD* available");
            return;
        }
        let dev = DrmDevice::open_first_render_node().unwrap();
        let handle = dev.create_binary_syncobj().expect("create");
        assert!(handle.raw() != 0);
        let fd = dev.handle_to_fd(&handle).expect("export fd");
        assert!(fd.as_raw_fd() >= 0);
        // Dropping the handle is safe even while the exported fd still
        // exists — kernel keeps a separate refcount per fd.
        drop(handle);
        // fd close happens on its own drop.
        drop(fd);
    }

    #[test]
    fn merge_sync_files_signaled_pair() {
        // End-to-end exercise of the reaper's fan-out merge path:
        //   create + signal two binary syncobjs → EXPORT_SYNC_FILE each
        //   → SYNC_IOC_MERGE → IMPORT_SYNC_FILE into a third syncobj
        //   → wait_signaled returns immediately.
        if !drm_available() {
            eprintln!("skip: no /dev/dri/renderD* available");
            return;
        }
        let dev = DrmDevice::open_first_render_node().unwrap();

        let h1 = dev.create_binary_syncobj().expect("create h1");
        let h2 = dev.create_binary_syncobj().expect("create h2");
        dev.signal(&h1).expect("signal h1");
        dev.signal(&h2).expect("signal h2");

        let sf1 = dev.export_sync_file(&h1).expect("export sf1");
        let sf2 = dev.export_sync_file(&h2).expect("export sf2");
        let merged = super::merge_sync_files(&sf1, &sf2).expect("merge");

        let h3 = dev.create_binary_syncobj().expect("create h3");
        dev.import_sync_file(&h3, &merged).expect("import merged");
        // Already-signaled inputs → merged is signaled → wait returns
        // immediately. timeout_nsec=0 (= "now" in absolute monotonic)
        // is enough.
        dev.wait_signaled(&h3, i64::MAX).expect("wait merged");
    }
}
