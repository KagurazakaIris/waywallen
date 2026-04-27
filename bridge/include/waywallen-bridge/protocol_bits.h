/* waywallen-bridge — protocol bit constants shared by every renderer
 * subprocess that participates in modifier negotiation v2.
 *
 * These values mirror waywallen/src/negotiate.rs (and the daemon's
 * `ConfigureBuffers` flag set). Producers OR them into the
 * `mem_hints` / `sync_caps` / `color_caps` scalars on
 * `ww_evt_format_caps_t` and check them on incoming
 * `ww_req_negotiate_buffers_t`. The daemon takes the intersection
 * with each consumer's caps to pick a scheme.
 *
 * Keep in sync with negotiate.rs — all bits are part of the wire
 * contract.
 */
#ifndef WAYWALLEN_BRIDGE_PROTOCOL_BITS_H
#define WAYWALLEN_BRIDGE_PROTOCOL_BITS_H

#ifdef __cplusplus
extern "C" {
#endif

/* `usages` per (fourcc, modifier): how the producer/consumer can use
 * the buffer. Producer sets bits the allocator can satisfy;
 * consumer sets bits its sampler/scanout path requires. */
#define WW_USAGE_SAMPLED          (1u << 0)
#define WW_USAGE_STORAGE          (1u << 1)
#define WW_USAGE_COLOR_ATTACHMENT (1u << 2)
#define WW_USAGE_TRANSFER_SRC     (1u << 4)
#define WW_USAGE_TRANSFER_DST     (1u << 5)

/* `mem_hints`: where the dmabuf is backed. The picker forces
 * HOST_VISIBLE on cross-GPU links so PRIME-import works on the
 * consumer side; same-GPU prefers DEVICE_LOCAL when available. */
#define WW_MEM_HINT_DEVICE_LOCAL  (1u << 0)
#define WW_MEM_HINT_HOST_VISIBLE  (1u << 1)

/* `sync_caps`: which fence flavours the peer supports. The picker
 * lands on the highest tier both sides advertise. */
#define WW_SYNC_DMABUF_IMPLICIT   (1u << 0)
#define WW_SYNC_SYNCOBJ_BINARY    (1u << 1)
#define WW_SYNC_SYNCOBJ_TIMELINE  (1u << 2)

/* `color_caps`: per-axis intersection (encoding | range | alpha). */
#define WW_COLOR_ENC_SRGB         (1u << 0)
#define WW_COLOR_ENC_BT709        (1u << 1)
#define WW_COLOR_ENC_BT2020       (1u << 2)
#define WW_COLOR_RANGE_FULL       (1u << 5)
#define WW_COLOR_RANGE_LIMITED    (1u << 6)
#define WW_COLOR_ALPHA_PREMUL     (1u << 7)
#define WW_COLOR_ALPHA_STRAIGHT   (1u << 8)

/* `flags` on `ww_evt_bind_buffers_t` / `ConfigureBuffers`. Bit 0
 * tells the consumer the dmabuf is backed by HOST_VISIBLE memory
 * (GTT) so a foreign GPU can PRIME-import it. */
#define WW_BUF_HOST_VISIBLE       (1u << 0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_BRIDGE_PROTOCOL_BITS_H */
