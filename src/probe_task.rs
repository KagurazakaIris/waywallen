//! Background media-probe scheduler.
//!
//! Decoupled from the scan/sync hot path: when an item lands in the DB
//! without media metadata (size/width/height/format), this module runs
//! the probe out-of-band on a periodic tick (and once after each sync).
 
use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use sea_orm::DatabaseConnection;
use tokio::sync::watch;

use crate::error::{Result, ResultExt};
use crate::media_probe::MediaProbe;
use crate::model::repo;
use crate::tasks::now_ms;

/// How often the scheduler wakes up to drain pending items.
pub const PROBE_TICK: Duration = Duration::from_secs(300);

/// Minimum gap between two probe attempts for the same item. Items
/// whose `sync_at` is newer than `now - PROBE_COOLDOWN` are skipped
/// even if their media columns are still NULL.
pub const PROBE_COOLDOWN: Duration = Duration::from_secs(6 * 60 * 60);

/// Hard cap on items processed per tick.
pub const PROBE_BATCH: usize = 64;

/// Larger cap used by the post-sync one-shot path so a fresh import is
/// drained quickly rather than one tick at a time.
pub const PROBE_REFRESH_BATCH: usize = 256;

/// Extensions we attempt to probe. Lowercased, no leading dot.
pub const PROBABLE_EXTS: &[&str] = &[
    "mp4", "mkv", "webm", "mov", "avi", "png", "jpg", "jpeg", "webp", "gif", "bmp", "tiff", "tif",
    "avif",
];

fn is_probable(path: &str) -> bool {
    Path::new(path)
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| {
            let lower = e.to_ascii_lowercase();
            PROBABLE_EXTS.iter().any(|p| *p == lower)
        })
        .unwrap_or(false)
}

/// Per-pass statistics. Returned by [`run_pending`] and emitted as a
/// structured info log line so operators can see at a glance how the
/// probe scheduler is progressing.
#[derive(Debug, Clone, Copy, Default)]
pub struct ProbeStats {
    /// Total items the cooldown query returned (pre extension filter).
    pub candidates: usize,
    /// Items skipped because their extension is not in [`PROBABLE_EXTS`].
    pub skipped_extension: usize,
    /// Items handed to `MediaProbe::probe`.
    pub probed: usize,
    /// Items where the probe returned at least one of width / height.
    pub gained_dimensions: usize,
    /// Items where the probe returned a non-empty `format` string.
    pub gained_format: usize,
    /// Items whose DB write failed; counted but not fatal to the pass.
    pub write_errors: usize,
    /// Wall time the pass took, in milliseconds.
    pub elapsed_ms: u128,
}

/// Drain up to `max` pending items in one pass. Returns per-pass
/// statistics. Always emits an info log line with the same numbers
/// when at least one candidate was considered, so the user can tail
/// the daemon log to see scheduler progress.
pub async fn run_pending(
    db: &DatabaseConnection,
    probe: Arc<dyn MediaProbe>,
    max: usize,
) -> Result<ProbeStats> {
    let mut stats = ProbeStats::default();
    if max == 0 {
        return Ok(stats);
    }
    let started = std::time::Instant::now();
    let cooldown_cutoff = now_ms() - PROBE_COOLDOWN.as_millis() as i64;
    // Pull a generous candidate window from DB then extension-filter
    // in Rust so the SQL stays portable. A multiplier of 4 is enough
    // for the practical mix of probable / non-probable items.
    let candidates =
        repo::list_items_pending_probe(db, cooldown_cutoff, (max as u64).saturating_mul(4)).await?;
    stats.candidates = candidates.len();

    for (item, library_root) in candidates {
        if stats.probed >= max {
            break;
        }
        if !is_probable(&item.path) {
            stats.skipped_extension += 1;
            continue;
        }
        let abs = join_path(&library_root, &item.path);
        let probe_for_blocking = probe.clone();
        let abs_for_blocking = abs.clone();
        let meta = tokio::task::spawn_blocking(move || {
            probe_for_blocking.probe(&abs_for_blocking)
        })
        .await
        .with_context(|| format!("probe join id={}", item.id))?;

        if meta.width.is_some() || meta.height.is_some() {
            stats.gained_dimensions += 1;
        }
        if meta.format.is_some() {
            stats.gained_format += 1;
        }

        if let Err(e) = repo::update_item_media(db, item.id, &meta).await {
            log::warn!(
                "probe write failed id={} path={}: {e:#}",
                item.id,
                abs
            );
            stats.write_errors += 1;
            continue;
        }
        stats.probed += 1;
    }

    stats.elapsed_ms = started.elapsed().as_millis();

    log::info!(
        target: "waywallen::probe_task",
        "probe pass done: candidates={} probed={} ext_skipped={} +dims={} +format={} errors={} took={}ms",
        stats.candidates,
        stats.probed,
        stats.skipped_extension,
        stats.gained_dimensions,
        stats.gained_format,
        stats.write_errors,
        stats.elapsed_ms,
    );
    Ok(stats)
}

/// Long-lived probe scheduler. Wakes every [`PROBE_TICK`] and drains
/// up to [`PROBE_BATCH`] items. Returns when `shutdown_rx` flips to
/// `true`.
pub async fn scheduler_loop(
    db: DatabaseConnection,
    probe: Arc<dyn MediaProbe>,
    mut shutdown_rx: watch::Receiver<bool>,
) -> Result<()> {
    log::info!(
        "probe scheduler started (tick={:?}, cooldown={:?}, batch={})",
        PROBE_TICK,
        PROBE_COOLDOWN,
        PROBE_BATCH
    );
    let mut interval = tokio::time::interval(PROBE_TICK);
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    // First tick fires immediately; let it run so newly-installed
    // items get probed promptly on daemon start.
    loop {
        tokio::select! {
            biased;
            res = shutdown_rx.changed() => {
                if res.is_err() || *shutdown_rx.borrow() {
                    log::info!("probe scheduler exiting (shutdown)");
                    return Ok(());
                }
            }
            _ = interval.tick() => {
                // run_pending logs its own structured info line on
                // every non-empty pass; we only need to surface failures.
                if let Err(e) = run_pending(&db, probe.clone(), PROBE_BATCH).await {
                    log::warn!("probe scheduler tick failed: {e:#}");
                }
            }
        }
    }
}

fn join_path(root: &str, rel: &str) -> String {
    let root = root.trim_end_matches('/');
    let rel = rel.trim_start_matches('/');
    if rel.is_empty() {
        root.to_owned()
    } else {
        format!("{root}/{rel}")
    }
}

// Re-export used by main.rs / control.rs so they don't need to know
// about the underlying HashMap detail.
#[allow(dead_code)]
pub(crate) type LibraryRootMap = HashMap<i64, String>;
