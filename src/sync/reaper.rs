//! Per-renderer release-fence reaper.
//!
//! Owns a Tokio task that drains [`FrameRecord`]s from
//! `display_endpoint::forward_frame_ready`. Each frame fanned out to a
//! consumer produces one record carrying:
//!
//!   - the producer-assigned `release_point` (the timeline value the
//!     producer's later submit will WAIT on before reusing the slot),
//!   - the daemon-allocated binary drm_syncobj handle for THIS consumer
//!     (whose fd was sent to that consumer; the consumer signals it
//!     from its release GPU work),
//!   - `expected_count` — the fan-out width: how many records will
//!     arrive for this `release_point` in total.
//!
//! The reaper buckets records by `release_point`. A bucket is flushed
//! when it has collected `expected_count` records OR when its bucket
//! deadline expires (`BUCKET_TIMEOUT`). On flush:
//!
//!   1. wait for every collected handle to signal (kernel ioctl,
//!      `WAIT_ALL` flag, deadline = `WAIT_TIMEOUT`).
//!   2. if any handle did not signal in time, force-SIGNAL it so the
//!      producer's wait at this `release_point` doesn't deadlock.
//!      The producer may then race against a still-reading consumer,
//!      but a stalled producer is worse than a stale read on a
//!      single-frame static image.
//!   3. TRANSFER from one of the (now-signaled) consumer handles onto
//!      the producer's release timeline at `release_point`. We do not
//!      `SYNC_IOC_MERGE` the per-consumer fences first because, by
//!      construction (step 1 just succeeded), every fence is already
//!      signaled — the producer's wait passes regardless of which one
//!      the timeline ends up holding. The merge would matter for
//!      profiling tools that want to see cumulative wait stats; not a
//!      concern in the prototype.
//!
//! The producer's release timeline syncobj is imported lazily into a
//! daemon-side handle on the first flushed bucket and cached.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use tokio::sync::mpsc;
use tokio::time::Instant;

use crate::renderer_manager::RendererHandle;
use crate::sync::drm_syncobj::{DrmDevice, SyncobjHandle};

/// Maximum age of a bucket before it gets force-flushed even if not
/// every consumer has reported in. Sized so a 60 fps producer can
/// burn through ~30 frames before back-pressuring; longer lets stuck
/// consumers stall the producer for too long.
const BUCKET_TIMEOUT: Duration = Duration::from_millis(500);

/// Per-handle wait deadline inside the bucket-flush ioctl. If a
/// consumer hasn't signaled by then we force-SIGNAL its handle (see
/// module docs).
const WAIT_TIMEOUT: Duration = Duration::from_millis(500);

/// Per-frame work item produced by `display_endpoint::forward_frame_ready`
/// and consumed by [`spawn_reaper`].
pub struct FrameRecord {
    pub release_point: u64,
    /// `None` means the router had zero enabled recipients for this
    /// frame (e.g. paused renderer, no displays linked). The reaper
    /// still has to advance the producer's release timeline at
    /// `release_point` — otherwise the producer's back-pressure
    /// `wait_release_point` will time out forever once it cycles back
    /// to this slot. With `None`, the reaper allocates a fresh
    /// already-signaled binary syncobj and TRANSFERs it onto the
    /// producer timeline directly, bypassing the bucket flow.
    pub consumer_handle: Option<SyncobjHandle>,
    /// Total fan-out width — how many records the reaper should expect
    /// to see with this `release_point` before flushing. `0` is only
    /// valid alongside `consumer_handle == None`.
    pub expected_count: u32,
}

struct Bucket {
    handles: Vec<SyncobjHandle>,
    expected: u32,
    deadline: Instant,
}

pub fn spawn_reaper(
    drm: &'static DrmDevice,
    renderer: Arc<RendererHandle>,
    mut rx: mpsc::UnboundedReceiver<FrameRecord>,
) {
    let renderer_id = renderer.id.clone();
    tokio::spawn(async move {
        let mut producer_handle: Option<SyncobjHandle> = None;
        let mut buckets: HashMap<u64, Bucket> = HashMap::new();

        loop {
            // Earliest bucket deadline. None when there are no pending
            // buckets, in which case we just wait on the channel.
            let next_deadline = buckets.values().map(|b| b.deadline).min();

            tokio::select! {
                maybe_record = rx.recv() => {
                    let Some(record) = maybe_record else {
                        // Channel closed (renderer evicted). Force-flush
                        // remaining buckets so we don't strand any
                        // consumer handles, then exit.
                        for (point, bucket) in buckets.drain() {
                            flush_bucket(drm, &renderer, &mut producer_handle, point, bucket).await;
                        }
                        log::info!("reaper {renderer_id}: channel closed, exiting");
                        return;
                    };
                    let Some(consumer_handle) = record.consumer_handle else {
                        // No real recipients (paused / no enabled outputs).
                        // Advance the producer's release timeline directly
                        // so its back-pressure wait at this point doesn't
                        // hang forever.
                        advance_release_point(
                            drm, &renderer, &mut producer_handle,
                            record.release_point,
                        ).await;
                        continue;
                    };
                    let entry = buckets.entry(record.release_point).or_insert_with(|| {
                        Bucket {
                            handles: Vec::new(),
                            expected: record.expected_count,
                            deadline: Instant::now() + BUCKET_TIMEOUT,
                        }
                    });
                    // Defensive: if a later record reports a different
                    // expected_count for the same release_point, take
                    // the larger value. Means the reaper will wait
                    // longer rather than flushing prematurely on an
                    // off-by-one race in the router.
                    entry.expected = entry.expected.max(record.expected_count);
                    entry.handles.push(consumer_handle);
                    if entry.handles.len() as u32 >= entry.expected {
                        let bucket = buckets.remove(&record.release_point).unwrap();
                        flush_bucket(drm, &renderer, &mut producer_handle, record.release_point, bucket).await;
                    }
                }
                _ = sleep_until_or_pending(next_deadline) => {
                    // Find every bucket whose deadline has passed and
                    // flush them. Snapshot the keys first to keep the
                    // iterator from outliving the mutable borrow.
                    let now = Instant::now();
                    let expired: Vec<u64> = buckets
                        .iter()
                        .filter(|(_, b)| b.deadline <= now)
                        .map(|(p, _)| *p)
                        .collect();
                    for point in expired {
                        let bucket = buckets.remove(&point).unwrap();
                        log::warn!(
                            "reaper {renderer_id}: bucket point {point} timed out \
                             with {}/{} consumer signals — force-flushing",
                            bucket.handles.len(),
                            bucket.expected,
                        );
                        flush_bucket(drm, &renderer, &mut producer_handle, point, bucket).await;
                    }
                }
            }
        }
    });
}

/// Sleep until `deadline`. If `deadline` is `None`, never resolve —
/// the surrounding `tokio::select!` falls through to the recv arm.
async fn sleep_until_or_pending(deadline: Option<Instant>) {
    match deadline {
        Some(d) => tokio::time::sleep_until(d).await,
        None => std::future::pending::<()>().await,
    }
}

/// Lazy-import the producer's release_syncobj into our handle cache.
/// Returns true if `producer_handle` is `Some` after this call.
fn ensure_producer_handle(
    drm: &'static DrmDevice,
    renderer: &Arc<RendererHandle>,
    producer_handle: &mut Option<SyncobjHandle>,
    release_point: u64,
) -> bool {
    if producer_handle.is_some() {
        return true;
    }
    let Some(fd) = renderer.clone_release_syncobj_fd() else {
        log::warn!(
            "reaper {}: dropping point {release_point} — \
             producer hasn't sent ReleaseSyncobj yet",
            renderer.id
        );
        return false;
    };
    match drm.fd_to_handle(&fd) {
        Ok(h) => {
            *producer_handle = Some(h);
            log::info!("reaper {}: imported release_syncobj", renderer.id);
            true
        }
        Err(e) => {
            log::warn!(
                "reaper {}: DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE failed: {e}",
                renderer.id
            );
            false
        }
    }
}

/// Used when the router has zero recipients for a frame: allocate a
/// fresh binary syncobj, force-SIGNAL it, and TRANSFER it onto the
/// producer's release timeline at `release_point`. The producer's
/// next back-pressure wait at this point therefore returns
/// immediately rather than timing out.
async fn advance_release_point(
    drm: &'static DrmDevice,
    renderer: &Arc<RendererHandle>,
    producer_handle: &mut Option<SyncobjHandle>,
    release_point: u64,
) {
    if !ensure_producer_handle(drm, renderer, producer_handle, release_point) {
        return;
    }
    let producer = producer_handle.as_ref().expect("set above");

    let placeholder = match drm.create_binary_syncobj() {
        Ok(h) => h,
        Err(e) => {
            log::warn!(
                "reaper {}: advance point {release_point}: create_binary_syncobj: {e}",
                renderer.id
            );
            return;
        }
    };
    if let Err(e) = drm.signal(&placeholder) {
        log::warn!(
            "reaper {}: advance point {release_point}: SIGNAL: {e}",
            renderer.id
        );
        return;
    }
    if let Err(e) = drm.transfer(&placeholder, 0, producer, release_point) {
        log::warn!(
            "reaper {}: advance point {release_point}: TRANSFER: {e}",
            renderer.id
        );
    }
    // `placeholder` drops here → DESTROY ioctl. Producer timeline
    // already holds the signaled fence via TRANSFER, so this is safe.
}

/// Wait for every handle in `bucket` to signal (force-SIGNAL stragglers
/// after `WAIT_TIMEOUT`), then TRANSFER one of them onto the producer's
/// release timeline at `release_point`. Imports the producer timeline
/// lazily on first call, caches in `producer_handle`.
async fn flush_bucket(
    drm: &'static DrmDevice,
    renderer: &Arc<RendererHandle>,
    producer_handle: &mut Option<SyncobjHandle>,
    release_point: u64,
    mut bucket: Bucket,
) {
    if bucket.handles.is_empty() {
        return;
    }

    if !ensure_producer_handle(drm, renderer, producer_handle, release_point) {
        return;
    }
    let producer = producer_handle.as_ref().expect("set above");

    // 1+2. Wait for all consumer signals; force-signal stragglers.
    let renderer_id = renderer.id.clone();
    // wait_handles_signaled wants ABSOLUTE CLOCK_MONOTONIC.
    // Compute now + WAIT_TIMEOUT in nsec.
    let timeout_nsec = {
        let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
        let ok = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) } == 0;
        if !ok {
            i64::MAX
        } else {
            (ts.tv_sec as i64)
                .checked_mul(1_000_000_000)
                .and_then(|s| s.checked_add(ts.tv_nsec as i64))
                .and_then(|now| now.checked_add(WAIT_TIMEOUT.as_nanos() as i64))
                .unwrap_or(i64::MAX)
        }
    };

    // Move ownership across spawn_blocking boundary to keep handles
    // alive on the blocking thread; ioctl needs &SyncobjHandle so we
    // re-borrow there.
    let handles_for_blocking = std::mem::take(&mut bucket.handles);
    let join = tokio::task::spawn_blocking(move || {
        let refs: Vec<&SyncobjHandle> = handles_for_blocking.iter().collect();
        let res = drm.wait_handles_signaled(&refs, timeout_nsec);
        (res, handles_for_blocking)
    })
    .await;
    let (wait_result, handles) = match join {
        Ok(pair) => pair,
        Err(e) => {
            log::warn!("reaper {renderer_id}: wait task panicked: {e}");
            return;
        }
    };

    if let Err(e) = wait_result {
        log::warn!(
            "reaper {renderer_id}: wait point {release_point} timed out / errored ({e}); \
             force-signaling stragglers"
        );
        for h in &handles {
            // SIGNAL is a CPU-side mark; cheap and cannot fail in any
            // meaningful way for our handles.
            if let Err(se) = drm.signal(h) {
                log::warn!("reaper {renderer_id}: force SIGNAL failed: {se}");
            }
        }
    }

    // 3. TRANSFER. Single-consumer fast path: skip the merge dance.
    let n = handles.len();
    if n == 1 {
        if let Err(e) = drm.transfer(&handles[0], 0, producer, release_point) {
            log::warn!(
                "reaper {renderer_id}: TRANSFER to point {release_point} failed: {e}"
            );
        } else {
            log::trace!(
                "reaper {renderer_id}: flushed point {release_point} (1 consumer)"
            );
        }
        drop(handles);
        return;
    }

    // Fan-out merge:
    //   3a. EXPORT_SYNC_FILE on each consumer handle.
    //   3b. Fold-merge sync_files via SYNC_IOC_MERGE.
    //   3c. Allocate a temp binary syncobj, IMPORT_SYNC_FILE the merge.
    //   3d. TRANSFER from temp into producer timeline at release_point.
    let mut sync_files: Vec<std::os::fd::OwnedFd> = Vec::with_capacity(n);
    let mut export_failed = false;
    for h in &handles {
        match drm.export_sync_file(h) {
            Ok(fd) => sync_files.push(fd),
            Err(e) => {
                log::warn!(
                    "reaper {renderer_id}: EXPORT_SYNC_FILE on point {release_point} failed: {e}"
                );
                export_failed = true;
                break;
            }
        }
    }
    if export_failed || sync_files.is_empty() {
        // Fall back to a single TRANSFER so the producer's wait at
        // this release_point still completes; we lose accurate fan-out
        // fence content but at least don't deadlock.
        if let Err(e) = drm.transfer(&handles[0], 0, producer, release_point) {
            log::warn!(
                "reaper {renderer_id}: fallback TRANSFER to point {release_point} failed: {e}"
            );
        }
        drop(handles);
        return;
    }

    let merged = sync_files
        .into_iter()
        .reduce(|a, b| match crate::sync::merge_sync_files(&a, &b) {
            Ok(m) => m,
            Err(e) => {
                log::warn!(
                    "reaper {renderer_id}: SYNC_IOC_MERGE on point {release_point} failed: {e}; \
                     dropping later fences"
                );
                a
            }
        })
        .expect("non-empty after empty-check above");

    let temp_handle = match drm.create_binary_syncobj() {
        Ok(h) => h,
        Err(e) => {
            log::warn!(
                "reaper {renderer_id}: create temp syncobj for point {release_point} failed: {e}"
            );
            drop(handles);
            return;
        }
    };
    if let Err(e) = drm.import_sync_file(&temp_handle, &merged) {
        log::warn!(
            "reaper {renderer_id}: IMPORT_SYNC_FILE for point {release_point} failed: {e}"
        );
        drop(handles);
        return;
    }
    if let Err(e) = drm.transfer(&temp_handle, 0, producer, release_point) {
        log::warn!(
            "reaper {renderer_id}: TRANSFER (merged) to point {release_point} failed: {e}"
        );
        return;
    }
    log::trace!(
        "reaper {renderer_id}: flushed point {release_point} ({n} consumer fences merged)"
    );
    // temp_handle, merged sync_file, and consumer handles all drop
    // here → kernel cleanup. The producer-side timeline holds the
    // merged fence (already signaled, since wait_all returned Ok).
    drop(handles);
}
