//! Modifier / format / sync / color negotiation engine.
//!
//! Pure data + pure functions. No locks, no I/O. Called from the
//! router whenever (renderer caps, consumer caps, link topology)
//! changes; the result is a [`NegotiatedScheme`] the daemon dispatches
//! to the renderer via `negotiate_buffers` and uses to gate
//! `bind_buffers` flow downstream.
//!
//! Wire format: parallel arrays decoded by [`unflatten_caps`]. Both
//! `format_caps` (renderer→daemon, ipc-v2) and `consumer_caps`
//! (consumer→daemon, display-v2) share the same shape; only the
//! `DeviceIdentity` source differs.
//!
//! Iter 1 (this file at landing time): scaffolding only. [`pick`]
//! always returns a deterministic `(ABGR8888, LINEAR, TIMELINE)`
//! scheme so the surrounding plumbing can be wired up without
//! depending on real peer probes. Iter 2 replaces [`pick`] with the
//! real intersection logic. Both UUID matching and per-axis
//! intersection helpers below are already wired up so iter 2 only
//! has to implement the policy.

use std::collections::{BTreeMap, HashSet};

use crate::renderer_manager::DrmNode;

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Per-(fourcc, modifier) capability descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ModCap {
    pub modifier: u64,
    pub usage: u32, // bitmask of USAGE_*
    pub plane_count: u32,
}

/// Pretty-print a 4-character fourcc as ASCII when printable, else
/// fall back to the raw hex literal. Used in cap logs so operators
/// can see `'AB24'` instead of `0x34324241`.
fn fourcc_str(fourcc: u32) -> String {
    let b = fourcc.to_le_bytes();
    if b.iter().all(|&c| (0x20..=0x7e).contains(&c)) {
        format!("'{}{}{}{}'", b[0] as char, b[1] as char, b[2] as char, b[3] as char)
    } else {
        format!("0x{fourcc:08x}")
    }
}

fn hex_uuid(bytes: &[u8; 16]) -> String {
    let mut s = String::with_capacity(32);
    for b in bytes {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

/// fourcc → modifier capability list.
#[derive(Debug, Clone, Default)]
pub struct FormatCaps {
    pub by_fourcc: BTreeMap<u32, Vec<ModCap>>,
}

/// Producer or consumer device identity. UUID source: Vulkan
/// `VkPhysicalDeviceIDProperties.{deviceUUID,driverUUID}`; DRM render
/// node from `VK_EXT_physical_device_drm` or
/// `EGL_DRM_RENDER_NODE_FILE_EXT`. All-zero UUID means "unknown" and
/// the picker falls back to DRM major:minor for same-device matching.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DeviceIdentity {
    pub device_uuid: [u8; 16],
    pub driver_uuid: [u8; 16],
    pub drm: DrmNode,
}

impl DeviceIdentity {
    pub const ZERO: Self = Self {
        device_uuid: [0; 16],
        driver_uuid: [0; 16],
        drm: DrmNode::UNKNOWN,
    };

    /// Whether two identities refer to the same physical GPU.
    ///
    /// UUID is *authoritative* when both sides have one — DRM minors
    /// can be remapped across container/namespace boundaries, while
    /// the Vulkan device UUID is stable. Falls back to DRM
    /// major:minor only when at least one side lacks a UUID. Returns
    /// false when neither path can produce a positive answer.
    pub fn same_device(&self, other: &Self) -> bool {
        let self_uuid_known = self.device_uuid != [0u8; 16];
        let other_uuid_known = other.device_uuid != [0u8; 16];
        if self_uuid_known && other_uuid_known {
            // Authoritative — trust UUID, ignore DRM mismatch.
            return self.device_uuid == other.device_uuid;
        }
        if self.drm.is_known() && other.drm.is_known() {
            return self.drm == other.drm;
        }
        false
    }
}

/// Combined capability set from a single peer (renderer or consumer).
#[derive(Debug, Clone)]
pub struct PeerCaps {
    pub formats: FormatCaps,
    pub identity: DeviceIdentity,
    pub sync: u32,
    pub color: u32,
    pub mem_hint: u32,
    pub extent_max: (u32, u32),
    /// (fourcc, modifier) pairs the daemon previously tried and the
    /// peer rejected via `bind_failed`. Filtered out at every `pick`
    /// call. Daemon owns mutation; engines are pure.
    pub blacklist: HashSet<(u32, u64)>,
}

impl PeerCaps {
    /// Multi-line dump of every advertised (fourcc, modifier) pair
    /// plus the secondary cap surface. Each line is logged at DEBUG
    /// with `prefix` (e.g. `"renderer R1: format_caps"` or
    /// `"display 7: consumer_caps"`) so an operator can see exactly
    /// what each peer told the daemon when running with
    /// `RUST_LOG=debug`. The one-line "imported N fourccs" summary
    /// at the call site stays at INFO so default-log operators
    /// still see arrival.
    pub fn log_dump(&self, prefix: &str) {
        log::debug!(
            "{prefix}: device_uuid={} driver_uuid={} drm_render={}:{} \
             sync=0x{:x} color=0x{:x} mem_hint=0x{:x} extent_max={}x{}",
            hex_uuid(&self.identity.device_uuid),
            hex_uuid(&self.identity.driver_uuid),
            self.identity.drm.major,
            self.identity.drm.minor,
            self.sync,
            self.color,
            self.mem_hint,
            self.extent_max.0,
            self.extent_max.1,
        );
        for (fourcc, mods) in &self.formats.by_fourcc {
            log::debug!(
                "{prefix}: fourcc={} ({}) — {} modifier{}",
                fourcc_str(*fourcc),
                format_args!("0x{:08x}", fourcc),
                mods.len(),
                if mods.len() == 1 { "" } else { "s" },
            );
            for m in mods {
                log::debug!(
                    "{prefix}:   modifier=0x{:016x} usage=0x{:x} planes={}",
                    m.modifier,
                    m.usage,
                    m.plane_count,
                );
            }
        }
    }
}

/// Resolved scheme that both peers will use until the next
/// renegotiation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NegotiatedScheme {
    pub fourcc: u32,
    pub modifier: u64,
    pub plane_count: u32,
    pub sync_mode: u32, // exactly one bit of SYNC_*
    pub color: u32,
    pub mem_hint: u32,
    pub extent: (u32, u32),
    pub count: u32, // pool size, daemon-chosen
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NegotiateError {
    NoFormatIntersection,
    NoSyncIntersection,
    /// Validation failures from [`unflatten_caps`].
    MalformedCaps(&'static str),
}

// ---------------------------------------------------------------------------
// Wire-bit constants. Mirrored on every renderer + consumer; both
// sides MUST cite the same numbers. Documented in
// `protocol/waywallen_ipc_v1.xml::format_caps` and
// `protocol/waywallen_display_v1.xml::consumer_caps`.
// ---------------------------------------------------------------------------

pub const USAGE_SAMPLED: u32 = 1 << 0;
pub const USAGE_STORAGE: u32 = 1 << 1;
pub const USAGE_COLOR_ATTACHMENT: u32 = 1 << 2;
pub const USAGE_DEPTH_STENCIL: u32 = 1 << 3;
pub const USAGE_TRANSFER_SRC: u32 = 1 << 4;
pub const USAGE_TRANSFER_DST: u32 = 1 << 5;
pub const USAGE_SCANOUT: u32 = 1 << 6;

pub const MEM_HINT_DEVICE_LOCAL: u32 = 1 << 0;
pub const MEM_HINT_HOST_VISIBLE: u32 = 1 << 1;
pub const MEM_HINT_SCANOUT_CAPABLE: u32 = 1 << 2;
pub const MEM_HINT_PROTECTED: u32 = 1 << 3;

pub const SYNC_IMPLICIT: u32 = 1 << 0;
pub const SYNC_SYNCOBJ_BINARY: u32 = 1 << 1;
pub const SYNC_SYNCOBJ_TIMELINE: u32 = 1 << 2;

// Color (packed):
//   bits 0..4  encoding bitset
//   bits 5..6  range bitset
//   bit  7     alpha PREMUL supported
//   bit  8     alpha STRAIGHT supported
pub const COLOR_ENC_SRGB: u32 = 1 << 0;
pub const COLOR_ENC_LINEAR: u32 = 1 << 1;
pub const COLOR_ENC_BT601: u32 = 1 << 2;
pub const COLOR_ENC_BT709: u32 = 1 << 3;
pub const COLOR_ENC_BT2020: u32 = 1 << 4;
pub const COLOR_RANGE_FULL: u32 = 1 << 5;
pub const COLOR_RANGE_LIMITED: u32 = 1 << 6;
pub const COLOR_ALPHA_PREMUL: u32 = 1 << 7;
pub const COLOR_ALPHA_STRAIGHT: u32 = 1 << 8;

/// Reasonable default for the prototype: sRGB, limited-range,
/// premultiplied. Used when intersection on any color axis is empty.
pub const DEFAULT_COLOR: u32 = COLOR_ENC_SRGB | COLOR_RANGE_LIMITED | COLOR_ALPHA_PREMUL;

// DRM modifier sentinels.
pub const DRM_FORMAT_MOD_LINEAR: u64 = 0;
pub const DRM_FORMAT_MOD_INVALID: u64 = u64::MAX;

// Canonical fourccs the prototype uses end-to-end.
pub const DRM_FORMAT_ABGR8888: u32 = 0x3432_4241; // 'AB24'
pub const DRM_FORMAT_XBGR8888: u32 = 0x3432_4258; // 'XB24'
pub const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241; // 'AR24'
pub const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258; // 'XR24'

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

/// Wire parallel arrays → structured [`PeerCaps`]. Single point of
/// schema-level validation: every length invariant the picker depends
/// on is enforced here so [`pick`] can stay a pure transform.
#[allow(clippy::too_many_arguments)]
pub fn unflatten_caps(
    fourccs: &[u32],
    mod_counts: &[u32],
    modifiers: &[u64],
    usages: &[u32],
    plane_counts: &[u32],
    device_uuid: &[u32],
    driver_uuid: &[u32],
    drm: DrmNode,
    sync: u32,
    color: u32,
    mem_hint: u32,
    extent_max: (u32, u32),
) -> Result<PeerCaps, NegotiateError> {
    if fourccs.len() != mod_counts.len() {
        return Err(NegotiateError::MalformedCaps(
            "fourccs.len() != mod_counts.len()",
        ));
    }
    let total: usize = mod_counts.iter().map(|&n| n as usize).sum();
    if modifiers.len() != total {
        return Err(NegotiateError::MalformedCaps(
            "modifiers.len() != sum(mod_counts)",
        ));
    }
    if usages.len() != total {
        return Err(NegotiateError::MalformedCaps(
            "usages.len() != sum(mod_counts)",
        ));
    }
    if plane_counts.len() != total {
        return Err(NegotiateError::MalformedCaps(
            "plane_counts.len() != sum(mod_counts)",
        ));
    }
    if device_uuid.len() != 4 || driver_uuid.len() != 4 {
        return Err(NegotiateError::MalformedCaps(
            "device_uuid/driver_uuid must be 4×u32 (16 bytes packed LE)",
        ));
    }

    let mut by_fourcc: BTreeMap<u32, Vec<ModCap>> = BTreeMap::new();
    let mut cursor = 0usize;
    for (i, &fourcc) in fourccs.iter().enumerate() {
        let n = mod_counts[i] as usize;
        let mut caps = Vec::with_capacity(n);
        for j in 0..n {
            caps.push(ModCap {
                modifier: modifiers[cursor + j],
                usage: usages[cursor + j],
                plane_count: plane_counts[cursor + j],
            });
        }
        cursor += n;
        // Defensive: a peer that lists the same fourcc twice gets its
        // entries merged; the last-written cap wins per modifier. Both
        // ends should never do this but the protocol doesn't forbid it.
        by_fourcc.entry(fourcc).or_default().extend(caps);
    }

    Ok(PeerCaps {
        formats: FormatCaps { by_fourcc },
        identity: DeviceIdentity {
            device_uuid: pack_uuid_words(device_uuid),
            driver_uuid: pack_uuid_words(driver_uuid),
            drm,
        },
        sync,
        color,
        mem_hint,
        extent_max,
        blacklist: HashSet::new(),
    })
}

fn pack_uuid_words(words: &[u32]) -> [u8; 16] {
    let mut out = [0u8; 16];
    for (i, w) in words.iter().take(4).enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    out
}

// ---------------------------------------------------------------------------
// Picker
// ---------------------------------------------------------------------------

/// Color sub-axis masks for per-axis intersection.
const COLOR_MASK_ENCODING: u32 = COLOR_ENC_SRGB
    | COLOR_ENC_LINEAR
    | COLOR_ENC_BT601
    | COLOR_ENC_BT709
    | COLOR_ENC_BT2020;
const COLOR_MASK_RANGE: u32 = COLOR_RANGE_FULL | COLOR_RANGE_LIMITED;
const COLOR_MASK_ALPHA: u32 = COLOR_ALPHA_PREMUL | COLOR_ALPHA_STRAIGHT;

/// Default pool size when both sides leave it unspecified.
const DEFAULT_POOL_COUNT: u32 = 3;

/// Pick the best intersection of two peers' capabilities at `extent`.
///
/// Format selection:
///   1. `same_device` + non-LINEAR modifier in the intersection
///      (after blacklist exclusion) → best non-LINEAR. Best = lowest
///      fourcc value, then lowest non-LINEAR modifier value
///      (deterministic; real GPUs converge after a single
///      negotiation so a richer scoring policy adds nothing for the
///      prototype).
///   2. Else if LINEAR is in the intersection → LINEAR. This is the
///      cross-vendor escape hatch — every in-tree driver supports
///      LINEAR import.
///   3. Else `Err(NoFormatIntersection)`.
///
/// Sync: `producer.sync & consumer.sync` then keep the highest bit
/// in priority order TIMELINE > BINARY > IMPLICIT.
///
/// Color: per-axis bitwise AND of `producer.color` and
/// `consumer.color`; pick the lowest set bit per axis. If an axis
/// has no overlap, fall back to that axis of [`DEFAULT_COLOR`].
///
/// Mem hint: same-device → `producer.mem_hint & consumer.mem_hint`,
/// preferring DEVICE_LOCAL when available. Cross-device → force
/// HOST_VISIBLE so foreign-GPU PRIME import works.
pub fn pick(
    producer: &PeerCaps,
    consumer: &PeerCaps,
    extent: (u32, u32),
) -> Result<NegotiatedScheme, NegotiateError> {
    let same_dev = producer.identity.same_device(&consumer.identity);
    let (fourcc, modifier, plane_count) =
        pick_format(&producer.formats, &consumer.formats, &producer.blacklist, &consumer.blacklist, same_dev)?;

    let sync_mode = pick_sync(producer.sync, consumer.sync)?;
    let color = pick_color(producer.color, consumer.color);
    let mem_hint = pick_mem_hint(producer.mem_hint, consumer.mem_hint, same_dev);

    Ok(NegotiatedScheme {
        fourcc,
        modifier,
        plane_count,
        sync_mode,
        color,
        mem_hint,
        extent,
        count: DEFAULT_POOL_COUNT,
    })
}

fn pick_format(
    producer: &FormatCaps,
    consumer: &FormatCaps,
    p_blacklist: &HashSet<(u32, u64)>,
    c_blacklist: &HashSet<(u32, u64)>,
    same_dev: bool,
) -> Result<(u32, u64, u32), NegotiateError> {
    // Walk fourccs in sorted order so the picker's choice is stable
    // across runs (BTreeMap iteration). Within a fourcc we walk
    // *producer order* so the producer's preference (its slot pool's
    // pinned modifier) lands first — without this, the daemon would
    // pick the lowest-numbered modifier and bounce through every
    // wrong one via `bind_failed` before converging on what the
    // producer actually allocated.
    let mut best_non_linear: Option<(u32, u64, u32)> = None;
    let mut linear_fallback: Option<(u32, u64, u32)> = None;

    for (&fourcc, p_mods) in producer.by_fourcc.iter() {
        let Some(c_mods) = consumer.by_fourcc.get(&fourcc) else {
            continue;
        };
        // Modifier intersection on this fourcc, excluding either
        // side's blacklist. Preserve producer order — the producer
        // ships its preferred modifier first; the first non-LINEAR
        // entry of the intersection is therefore what the producer
        // pre-allocated against.
        let mut intersect: Vec<(u64, u32)> = Vec::new(); // (modifier, plane_count)
        for pc in p_mods {
            if p_blacklist.contains(&(fourcc, pc.modifier)) {
                continue;
            }
            for cc in c_mods {
                if c_blacklist.contains(&(fourcc, cc.modifier)) {
                    continue;
                }
                if pc.modifier == cc.modifier {
                    // plane_count must agree across sides; if it doesn't
                    // the renderer can't allocate something the consumer
                    // can import — skip.
                    if pc.plane_count != cc.plane_count {
                        continue;
                    }
                    intersect.push((pc.modifier, pc.plane_count));
                    break;
                }
            }
        }

        // Same-device + non-LINEAR available? Take the first
        // non-LINEAR in producer order. We prefer non-LINEAR strictly
        // when same-device because tiled/compressed formats are
        // usually a perf win.
        if same_dev {
            if let Some(&(m, pc)) = intersect.iter().find(|(m, _)| *m != DRM_FORMAT_MOD_LINEAR) {
                // Score: prefer the lowest fourcc value seen so far.
                if best_non_linear
                    .map(|(prev_fourcc, _, _)| fourcc < prev_fourcc)
                    .unwrap_or(true)
                {
                    best_non_linear = Some((fourcc, m, pc));
                }
                continue;
            }
        }
        // Linear fallback if available on this fourcc.
        if let Some(&(_, pc)) = intersect.iter().find(|(m, _)| *m == DRM_FORMAT_MOD_LINEAR) {
            if linear_fallback
                .map(|(prev_fourcc, _, _)| fourcc < prev_fourcc)
                .unwrap_or(true)
            {
                linear_fallback = Some((fourcc, DRM_FORMAT_MOD_LINEAR, pc));
            }
        }
    }

    best_non_linear
        .or(linear_fallback)
        .ok_or(NegotiateError::NoFormatIntersection)
}

fn pick_sync(producer: u32, consumer: u32) -> Result<u32, NegotiateError> {
    let common = producer & consumer;
    if common == 0 {
        return Err(NegotiateError::NoSyncIntersection);
    }
    // Priority order — keep ONE bit set.
    if common & SYNC_SYNCOBJ_TIMELINE != 0 {
        Ok(SYNC_SYNCOBJ_TIMELINE)
    } else if common & SYNC_SYNCOBJ_BINARY != 0 {
        Ok(SYNC_SYNCOBJ_BINARY)
    } else {
        Ok(SYNC_IMPLICIT)
    }
}

fn pick_color(producer: u32, consumer: u32) -> u32 {
    let intersect = producer & consumer;
    let pick_axis = |mask: u32| -> u32 {
        let common = intersect & mask;
        if common != 0 {
            // Lowest set bit — deterministic.
            common & common.wrapping_neg()
        } else {
            DEFAULT_COLOR & mask
        }
    };
    pick_axis(COLOR_MASK_ENCODING) | pick_axis(COLOR_MASK_RANGE) | pick_axis(COLOR_MASK_ALPHA)
}

fn pick_mem_hint(producer: u32, consumer: u32, same_dev: bool) -> u32 {
    if !same_dev {
        // Cross-device: force HOST_VISIBLE regardless of producer pref.
        // Foreign-GPU PRIME import fails on DEVICE_LOCAL VRAM.
        return MEM_HINT_HOST_VISIBLE;
    }
    let common = producer & consumer;
    if common & MEM_HINT_DEVICE_LOCAL != 0 {
        MEM_HINT_DEVICE_LOCAL
    } else if common & MEM_HINT_HOST_VISIBLE != 0 {
        MEM_HINT_HOST_VISIBLE
    } else if common != 0 {
        // Some other bit set (PROTECTED, SCANOUT_CAPABLE) — keep it.
        common
    } else {
        // Nothing in common. Cross-device path is excluded above; on
        // same-device with empty intersection we have to make a guess.
        // HOST_VISIBLE is the safer default (system memory always
        // works); tagged with a warning at the call site.
        MEM_HINT_HOST_VISIBLE
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn drm() -> DrmNode {
        DrmNode::UNKNOWN
    }

    #[test]
    fn unflatten_single_fourcc_single_modifier() {
        let caps = unflatten_caps(
            &[DRM_FORMAT_ABGR8888],
            &[1],
            &[DRM_FORMAT_MOD_LINEAR],
            &[USAGE_SAMPLED | USAGE_TRANSFER_DST],
            &[1],
            &[0; 4],
            &[0; 4],
            drm(),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
            (1920, 1080),
        )
        .unwrap();
        let mods = caps.formats.by_fourcc.get(&DRM_FORMAT_ABGR8888).unwrap();
        assert_eq!(mods.len(), 1);
        assert_eq!(mods[0].modifier, DRM_FORMAT_MOD_LINEAR);
        assert_eq!(mods[0].plane_count, 1);
        assert_eq!(caps.extent_max, (1920, 1080));
    }

    #[test]
    fn unflatten_multi_fourcc_multi_modifier() {
        // 2 fourccs: ABGR8888 with [LINEAR, INVALID]; XRGB8888 with [LINEAR]
        let caps = unflatten_caps(
            &[DRM_FORMAT_ABGR8888, DRM_FORMAT_XRGB8888],
            &[2, 1],
            &[
                DRM_FORMAT_MOD_LINEAR,
                DRM_FORMAT_MOD_INVALID,
                DRM_FORMAT_MOD_LINEAR,
            ],
            &[USAGE_SAMPLED, USAGE_SAMPLED, USAGE_SCANOUT],
            &[1, 1, 1],
            &[0; 4],
            &[0; 4],
            drm(),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            0,
            (640, 360),
        )
        .unwrap();
        assert_eq!(caps.formats.by_fourcc.len(), 2);
        let abgr = caps.formats.by_fourcc.get(&DRM_FORMAT_ABGR8888).unwrap();
        assert_eq!(abgr.len(), 2);
        assert_eq!(abgr[1].modifier, DRM_FORMAT_MOD_INVALID);
        let xrgb = caps.formats.by_fourcc.get(&DRM_FORMAT_XRGB8888).unwrap();
        assert_eq!(xrgb.len(), 1);
        assert_eq!(xrgb[0].usage, USAGE_SCANOUT);
    }

    #[test]
    fn unflatten_rejects_length_mismatch() {
        let err = unflatten_caps(
            &[DRM_FORMAT_ABGR8888],
            &[2],
            &[DRM_FORMAT_MOD_LINEAR], // sum(mod_counts) = 2, but only 1 modifier
            &[USAGE_SAMPLED, USAGE_SAMPLED],
            &[1, 1],
            &[0; 4],
            &[0; 4],
            drm(),
            0,
            0,
            0,
            (0, 0),
        )
        .unwrap_err();
        assert!(matches!(err, NegotiateError::MalformedCaps(_)));
    }

    #[test]
    fn unflatten_rejects_bad_uuid_length() {
        let err = unflatten_caps(
            &[],
            &[],
            &[],
            &[],
            &[],
            &[0, 0, 0], // only 3 words instead of 4
            &[0; 4],
            drm(),
            0,
            0,
            0,
            (0, 0),
        )
        .unwrap_err();
        assert!(matches!(err, NegotiateError::MalformedCaps(_)));
    }

    #[test]
    fn unflatten_rejects_bad_mod_counts() {
        let err = unflatten_caps(
            &[DRM_FORMAT_ABGR8888, DRM_FORMAT_XRGB8888],
            &[1], // length mismatch with fourccs
            &[DRM_FORMAT_MOD_LINEAR],
            &[USAGE_SAMPLED],
            &[1],
            &[0; 4],
            &[0; 4],
            drm(),
            0,
            0,
            0,
            (0, 0),
        )
        .unwrap_err();
        assert!(matches!(err, NegotiateError::MalformedCaps(_)));
    }

    #[test]
    fn unflatten_packs_uuid_le() {
        let caps = unflatten_caps(
            &[],
            &[],
            &[],
            &[],
            &[],
            // 0xAA in byte 0 of word 0 → out[0] should be 0xAA
            &[0xAA, 0, 0, 0],
            &[0, 0, 0xCAFE_BABE, 0],
            drm(),
            0,
            0,
            0,
            (0, 0),
        )
        .unwrap();
        assert_eq!(caps.identity.device_uuid[0], 0xAA);
        // word 2 = 0xCAFE_BABE → bytes 8..12 = [0xBE, 0xBA, 0xFE, 0xCA] (LE)
        assert_eq!(
            &caps.identity.driver_uuid[8..12],
            &[0xBE, 0xBA, 0xFE, 0xCA]
        );
    }

    #[test]
    fn device_identity_same_device_by_uuid() {
        let mut a = DeviceIdentity::ZERO;
        let mut b = DeviceIdentity::ZERO;
        a.device_uuid[0] = 0x42;
        b.device_uuid[0] = 0x42;
        assert!(a.same_device(&b));
    }

    #[test]
    fn device_identity_zero_uuid_falls_back_to_drm() {
        let a = DeviceIdentity {
            device_uuid: [0; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 128 },
        };
        let b = DeviceIdentity {
            device_uuid: [0; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 128 },
        };
        assert!(a.same_device(&b));
        let c = DeviceIdentity {
            device_uuid: [0; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 129 },
        };
        assert!(!a.same_device(&c));
    }

    #[test]
    fn device_identity_uuid_takes_precedence_over_mismatched_drm() {
        let mut a = DeviceIdentity {
            device_uuid: [0x42; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 128 },
        };
        let b = DeviceIdentity {
            device_uuid: [0x42; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 129 },
        };
        assert!(a.same_device(&b));
        // also: differing UUID is not same_device even if DRM matches.
        a.device_uuid = [0x42; 16];
        let c = DeviceIdentity {
            device_uuid: [0x99; 16],
            driver_uuid: [0; 16],
            drm: DrmNode { major: 226, minor: 128 },
        };
        assert!(!a.same_device(&c));
    }

    /// Build a PeerCaps with a single fourcc + a list of (modifier, plane_count) pairs.
    fn caps_one_fourcc(
        fourcc: u32,
        mods: &[(u64, u32)],
        identity: DeviceIdentity,
        sync: u32,
        color: u32,
        mem: u32,
    ) -> PeerCaps {
        let mod_count = mods.len() as u32;
        let modifiers: Vec<u64> = mods.iter().map(|(m, _)| *m).collect();
        let plane_counts: Vec<u32> = mods.iter().map(|(_, p)| *p).collect();
        let usages: Vec<u32> = vec![USAGE_SAMPLED; mods.len()];
        // device_uuid words from identity
        let dev_words: Vec<u32> = identity
            .device_uuid
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        let drv_words: Vec<u32> = identity
            .driver_uuid
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        unflatten_caps(
            &[fourcc],
            &[mod_count],
            &modifiers,
            &usages,
            &plane_counts,
            &dev_words,
            &drv_words,
            identity.drm,
            sync,
            color,
            mem,
            (1920, 1080),
        )
        .unwrap()
    }

    fn ident_uuid(byte: u8) -> DeviceIdentity {
        DeviceIdentity {
            device_uuid: [byte; 16],
            driver_uuid: [byte; 16],
            drm: DrmNode { major: 226, minor: 128 },
        }
    }

    #[test]
    fn pick_same_device_prefers_non_linear() {
        // Same UUID; both advertise LINEAR + a non-LINEAR modifier.
        let nl: u64 = 0x0100_0000_0000_0001;
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_DEVICE_LOCAL | MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_DEVICE_LOCAL | MEM_HINT_HOST_VISIBLE,
        );
        let s = pick(&p, &c, (640, 360)).unwrap();
        assert_eq!(s.fourcc, DRM_FORMAT_ABGR8888);
        assert_eq!(s.modifier, nl);
        assert_eq!(s.plane_count, 1);
        // Same device and DEVICE_LOCAL on both → DEVICE_LOCAL.
        assert_eq!(s.mem_hint, MEM_HINT_DEVICE_LOCAL);
    }

    #[test]
    fn pick_cross_device_falls_back_to_linear_and_forces_host_visible() {
        // Different UUIDs; both advertise LINEAR + a non-LINEAR
        // modifier. The non-LINEAR is theoretically common in value
        // but cross-vendor it's never actually portable, so the
        // policy ignores it and picks LINEAR.
        let nl: u64 = 0x0100_0000_0000_0001;
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0xAA),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_DEVICE_LOCAL,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0xBB),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let s = pick(&p, &c, (640, 360)).unwrap();
        assert_eq!(s.modifier, DRM_FORMAT_MOD_LINEAR);
        // Cross-device → HOST_VISIBLE forced regardless of producer pref.
        assert_eq!(s.mem_hint, MEM_HINT_HOST_VISIBLE);
    }

    #[test]
    fn pick_no_fourcc_intersection() {
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_XRGB8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        assert_eq!(
            pick(&p, &c, (640, 360)).unwrap_err(),
            NegotiateError::NoFormatIntersection
        );
    }

    #[test]
    fn pick_blacklist_excludes_modifier() {
        // Same device, both advertise non-LINEAR + LINEAR. Blacklist
        // the non-LINEAR on producer side → picker falls back to LINEAR.
        let nl: u64 = 0x0100_0000_0000_0001;
        let mut p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1), (nl, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        p.blacklist.insert((DRM_FORMAT_ABGR8888, nl));
        let s = pick(&p, &c, (640, 360)).unwrap();
        assert_eq!(s.modifier, DRM_FORMAT_MOD_LINEAR);
    }

    #[test]
    fn pick_sync_priority_timeline_over_binary() {
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE | SYNC_SYNCOBJ_BINARY | SYNC_IMPLICIT,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE | SYNC_SYNCOBJ_BINARY,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let s = pick(&p, &c, (640, 360)).unwrap();
        assert_eq!(s.sync_mode, SYNC_SYNCOBJ_TIMELINE);

        // Drop TIMELINE on consumer → BINARY wins.
        let c2 = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_BINARY | SYNC_IMPLICIT,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let s2 = pick(&p, &c2, (640, 360)).unwrap();
        assert_eq!(s2.sync_mode, SYNC_SYNCOBJ_BINARY);
    }

    #[test]
    fn pick_no_sync_intersection() {
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_IMPLICIT,
            DEFAULT_COLOR,
            MEM_HINT_HOST_VISIBLE,
        );
        assert_eq!(
            pick(&p, &c, (640, 360)).unwrap_err(),
            NegotiateError::NoSyncIntersection
        );
    }

    #[test]
    fn pick_color_per_axis_intersect() {
        // Producer: BT709 + full range + premul; Consumer: BT709 +
        // limited range + premul. Encoding axis intersect = BT709,
        // range axis intersect = empty → falls back to DEFAULT range
        // (LIMITED).
        let p_color = COLOR_ENC_BT709 | COLOR_RANGE_FULL | COLOR_ALPHA_PREMUL;
        let c_color = COLOR_ENC_BT709 | COLOR_RANGE_LIMITED | COLOR_ALPHA_PREMUL;
        let p = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            p_color,
            MEM_HINT_HOST_VISIBLE,
        );
        let c = caps_one_fourcc(
            DRM_FORMAT_ABGR8888,
            &[(DRM_FORMAT_MOD_LINEAR, 1)],
            ident_uuid(0x42),
            SYNC_SYNCOBJ_TIMELINE,
            c_color,
            MEM_HINT_HOST_VISIBLE,
        );
        let s = pick(&p, &c, (640, 360)).unwrap();
        assert!(s.color & COLOR_ENC_BT709 != 0, "BT709 must be picked");
        assert!(
            s.color & COLOR_RANGE_LIMITED != 0,
            "range axis empty → DEFAULT_COLOR's LIMITED applied"
        );
        assert!(s.color & COLOR_ALPHA_PREMUL != 0);
    }
}
