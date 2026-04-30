// waywallen-image-renderer — FFmpeg-decoded still image renderer subprocess.
//
// All DMA-BUF allocation + modifier negotiation + drm_syncobj sync lives
// in <waywallen-bridge/pool.h>. This plugin owns:
//   - Vulkan instance + physical device + device + queue (for upload)
//   - Staging buffer + command buffer (uploads RGBA into a bridge slot)
//   - libav decode pipeline

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/drm_fourcc.h>
#include <waywallen-bridge/ipc_v1.h>
#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/probe_vk.h>

#include "av_image.hpp"
#include "vk_producer.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct Options {
    std::string ipc_path;
    std::string image_path;
    uint32_t    width { 1920 };
    uint32_t    height { 1080 };
    bool        decode_only { false };
    bool        vulkan_probe { false };
    // Test hook: probe the picked Vulkan device for supported (fourcc,
    // modifier) pairs and emit a `PeerCaps`-shaped JSON document on
    // stdout, then exit. Consumed by the dmabuf_roundtrip_e2e test
    // orchestrator to compute the producer × consumer cap intersection
    // before per-pair iteration.
    bool        print_caps { false };
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-image-renderer: %s\n", msg.c_str());
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (a == "--ipc")              o.ipc_path = next();
        else if (a == "--width")       o.width = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--height")      o.height = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--image" || a == "--path") o.image_path = next();
        else if (a == "--decode-only") o.decode_only = true;
        else if (a == "--vulkan-probe") o.vulkan_probe = true;
        else if (a == "--print-caps")  o.print_caps = true;
        else ww_bridge_skip_unknown_kv_arg(&i, argc, argv);
    }
    return o;
}


struct HostState {
    int                    sock { -1 };
    ww_pool_t             *pool { nullptr };
    std::atomic<bool>      shutdown { false };
    std::atomic<bool>      negotiated { false };

    /* Reader → main negotiate handoff. */
    std::mutex             neg_mu;
    std::condition_variable neg_cv;
    bool                   neg_pending { false };
    ww_pool_directive_t    neg_directive {};

    /* Cached RGBA buffer (kept alive across re-negotiations so we
     * can re-upload after a directive change). */
    const uint8_t*         rgba_data { nullptr };
    size_t                 rgba_size { 0 };
};

void signal_shutdown(HostState& s) {
    s.shutdown.store(true, std::memory_order_release);
    s.neg_cv.notify_all();
}

// Test hook: when WAYWALLEN_IMAGE_DUMP_DIR is set, write the RGBA8
// bytes the renderer is about to upload to the GPU to a file the
// orchestrator can compare against the consumer-side dump. The dump
// captures the *input* (post-decode, pre-staging) so it's always
// linear regardless of the picked DRM modifier — the consumer also
// dumps post-readback linear bytes, so byte-equality is meaningful.
//
// Filename: producer-{seq:06}-0x{fourcc:08x}-0x{modifier:016x}.bin
// Sidecar:  same name with .json — width/height/stride/fourcc/modifier.
static void maybe_dump_producer_frame(const HostState& host,
                                      const ww_pool_directive_t& d,
                                      const ww_pool_slot_t& s,
                                      uint64_t seq) {
    const char* dir = std::getenv("WAYWALLEN_IMAGE_DUMP_DIR");
    if (!dir || !*dir) return;
    if (!host.rgba_data || host.rgba_size == 0) return;

    char path[512];
    std::snprintf(path, sizeof(path),
                  "%s/producer-%06llu-0x%08x-0x%016llx.bin",
                  dir,
                  static_cast<unsigned long long>(seq),
                  d.fourcc,
                  static_cast<unsigned long long>(d.modifier));
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: dump open %s: %s\n",
                     path, std::strerror(errno));
        return;
    }
    size_t w = std::fwrite(host.rgba_data, 1, host.rgba_size, f);
    std::fclose(f);
    if (w != host.rgba_size) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: dump short write %zu/%zu to %s\n",
                     w, host.rgba_size, path);
        return;
    }

    char sidecar[520];
    std::snprintf(sidecar, sizeof(sidecar),
                  "%s/producer-%06llu-0x%08x-0x%016llx.json",
                  dir,
                  static_cast<unsigned long long>(seq),
                  d.fourcc,
                  static_cast<unsigned long long>(d.modifier));
    FILE* sf = std::fopen(sidecar, "w");
    if (!sf) return;
    // Note: the dump is always tightly-packed RGBA8 (`width*height*4`
    // bytes) — that's the input format `decode_to_rgba` produces and
    // what `upload_into` accepts. The DMA-BUF stride/plane_offset are
    // the *destination* layout in the GPU buffer, which the consumer
    // reads back into the same tightly-packed shape; both sides' dumps
    // are therefore directly comparable.
    std::fprintf(sf,
                 "{\n"
                 "  \"kind\": \"producer\",\n"
                 "  \"seq\": %llu,\n"
                 "  \"fourcc\": \"0x%08x\",\n"
                 "  \"modifier\": \"0x%016llx\",\n"
                 "  \"width\": %u,\n"
                 "  \"height\": %u,\n"
                 "  \"stride\": %u,\n"
                 "  \"plane_offset\": %u,\n"
                 "  \"size\": %u,\n"
                 "  \"row_bytes\": %u,\n"
                 "  \"row_count\": %u,\n"
                 "  \"dump_layout\": \"tightly_packed_rgba8\"\n"
                 "}\n",
                 static_cast<unsigned long long>(seq),
                 d.fourcc,
                 static_cast<unsigned long long>(d.modifier),
                 s.width, s.height, s.stride, s.plane_offset, s.size,
                 s.width * 4u, s.height);
    std::fclose(sf);
}

bool upload_to_slot(HostState& host, ww_image::VkProducer& producer,
                    const ww_pool_directive_t& directive,
                    uint32_t slot_index) {
    ww_pool_slot_t s {};
    if (int rc = ww_bridge_pool_acquire_slot(host.pool, slot_index, &s);
        rc != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: acquire_slot(%u) failed: %d\n",
                     slot_index, rc);
        return false;
    }
    if (!s.vk_image) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: slot %u has no VkImage handle\n",
                     slot_index);
        return false;
    }

    static std::atomic<uint64_t> g_dump_seq { 0 };
    maybe_dump_producer_frame(host, directive, s,
                              g_dump_seq.fetch_add(1, std::memory_order_relaxed));

    std::string uerr;
    int sync_fd = producer.upload_into(
        reinterpret_cast<VkImage>(s.vk_image),
        s.width, s.height,
        host.rgba_data, host.rgba_size, &uerr);
    if (sync_fd < 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: upload_into failed: %s\n",
                     uerr.c_str());
        return false;
    }
    if (int rc = ww_bridge_pool_submit_slot(host.pool, host.sock, slot_index, sync_fd);
        rc != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: submit_slot rc=%d\n", rc);
        return false;
    }
    return true;
}

/* Apply a directive received from the daemon. After bridge brings the
 * slots up, upload our cached RGBA into slot 0 and submit one frame.
 * Static images: a single submit per (re-)negotiation is enough. */
void apply_negotiate_request(HostState& host, ww_image::VkProducer& producer,
                             const ww_pool_directive_t& d) {
    int rc = ww_bridge_pool_apply_directive(host.pool, host.sock, &d);
    if (rc != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: pool_apply_directive failed: %d\n", rc);
        if (rc > 0) signal_shutdown(host);
        return;
    }
    if (!upload_to_slot(host, producer, d, 0)) {
        signal_shutdown(host);
        return;
    }
    host.negotiated.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "waywallen-image-renderer: NegotiateBuffers honored "
                 "(path=%u mem_source=%u modifier=0x%016llx) — bind+frame emitted\n",
                 d.category, d.mem_source,
                 static_cast<unsigned long long>(d.modifier));
}

void apply_control(HostState& host, const ww_bridge_control_t& c) {
    switch (c.op) {
    case WW_REQ_HELLO:
    case WW_REQ_PLAY:
    case WW_REQ_PAUSE:
    case WW_REQ_MOUSE:
    case WW_REQ_SET_FPS:
        break;
    case WW_REQ_LOAD_SCENE:
        std::fprintf(stderr,
                     "waywallen-image-renderer: load_scene pkg=%s "
                     "(hot-swap not yet implemented)\n",
                     c.u.load_scene.pkg ? c.u.load_scene.pkg : "(null)");
        break;
    case WW_REQ_SHUTDOWN:
        signal_shutdown(host);
        break;
    case WW_REQ_NEGOTIATE_BUFFERS: {
        const auto& nb = c.u.negotiate_buffers;
        ww_pool_directive_t d {};
        d.category    = nb.path;
        d.mem_source  = nb.mem_source;
        d.fourcc      = nb.fourcc;
        d.modifier    = nb.modifier;
        d.plane_count = nb.plane_count;
        d.sync_mode   = nb.sync_mode;
        d.color       = nb.color;
        d.mem_hint    = nb.mem_hint;
        d.width       = nb.extent_w;
        d.height      = nb.extent_h;
        /* Static image: one slot is enough. */
        d.count       = 1;
        {
            std::lock_guard<std::mutex> lk(host.neg_mu);
            host.neg_directive = d;
            host.neg_pending = true;
        }
        host.neg_cv.notify_all();
        break;
    }
    default:
        std::fprintf(stderr,
                     "waywallen-image-renderer: unknown control op %d\n",
                     static_cast<int>(c.op));
        break;
    }
}

void reader_loop(HostState& host) {
    while (!host.shutdown.load(std::memory_order_acquire)) {
        ww_bridge_control_t msg {};
        int rc = ww_bridge_recv_control(host.sock, &msg);
        if (rc != 0) {
            if (!host.shutdown.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                             "waywallen-image-renderer: recv_control failed: %d\n",
                             rc);
            }
            signal_shutdown(host);
            return;
        }
        apply_control(host, msg);
        ww_bridge_control_free(&msg);
    }
}

// ---------------------------------------------------------------------------
// --print-caps
// ---------------------------------------------------------------------------

// Emit a single JSON document on stdout that mirrors the
// `PeerCapsJson` shape consumed by `dmabuf_roundtrip_e2e`. Hand-rolled
// (no nlohmann dep) because the schema is tiny and stable. Keep the
// field names and ordering in sync with
// `displays/dump-test/src/main.rs::PeerCapsJson`.
//
// We don't have a public "query caps without a socket" entry point on
// the bridge pool; instead we build a Vulkan pool, hand it one end of
// a `socketpair(AF_UNIX)`, ask it to advertise, then drain the
// `format_caps` message on the other end and decode it.
static int print_caps_json(const Options& opt) {
    std::string verr;
    auto producer = ww_image::VkProducer::create(opt.width, opt.height, &verr);
    if (!producer) {
        std::fprintf(stderr, "waywallen-image-renderer: vk_producer: %s\n",
                     verr.c_str());
        return 1;
    }

    int sv[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: socketpair: %s\n",
                     std::strerror(errno));
        return 1;
    }

    ww_pool_vulkan_init_t pool_init {};
    pool_init.instance              = producer->instance();
    pool_init.physical_device       = producer->physical_device();
    pool_init.device                = producer->device();
    pool_init.queue                 = producer->queue();
    pool_init.queue_family_index    = producer->queue_family_index();
    pool_init.get_instance_proc_addr =
        reinterpret_cast<void *(*)(void *, const char *)>(vkGetInstanceProcAddr);
    pool_init.device_uuid           = producer->device_uuid();
    pool_init.driver_uuid           = producer->driver_uuid();
    pool_init.drm_render_major      = producer->drm_render_major();
    pool_init.drm_render_minor      = producer->drm_render_minor();
    pool_init.drm_render_fd         = producer->drm_render_fd();

    ww_pool_t* pool = nullptr;
    if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pool_init, &pool);
        rc != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: pool_create: %d\n", rc);
        ::close(sv[0]); ::close(sv[1]);
        return 1;
    }

    if (int rc = ww_bridge_pool_advertise_caps(pool, sv[0],
                                               opt.width, opt.height,
                                               WW_MEM_HINT_DEVICE_LOCAL
                                               | WW_MEM_HINT_HOST_VISIBLE);
        rc != 0) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: advertise_caps: %d\n", rc);
        ww_bridge_pool_destroy(pool);
        ::close(sv[0]); ::close(sv[1]);
        return 1;
    }

    /* Drain frames on sv[1] until we get the FormatCaps. The pool
     * writes (in order): Ready, ReleaseSyncobj (with a syncobj fd),
     * FormatCaps. */
    ww_evt_format_caps_t caps {};
    bool got_caps = false;
    for (int frame = 0; frame < 6 && !got_caps; ++frame) {
        uint16_t op = 0;
        uint8_t* body = nullptr;
        size_t body_len = 0;
        int fds[2] = { -1, -1 };
        size_t n_fds = 0;
        int rc = ww_bridge_recv_frame(sv[1], &op, &body, &body_len,
                                      fds, 2, &n_fds);
        if (rc != 0) {
            std::fprintf(stderr,
                         "waywallen-image-renderer: recv_frame: %d\n", rc);
            break;
        }
        for (size_t i = 0; i < n_fds; ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
        }
        if (op == WW_EVT_FORMAT_CAPS) {
            if (ww_evt_format_caps_decode(body, body_len, &caps) == 0) {
                got_caps = true;
            }
        }
        free(body);
    }

    ww_bridge_pool_destroy(pool);
    ::close(sv[0]); ::close(sv[1]);

    if (!got_caps) {
        std::fprintf(stderr,
                     "waywallen-image-renderer: did not observe FormatCaps\n");
        return 1;
    }

    auto put_uuid = [](const ww_array_u32_t& a) -> std::string {
        // device_uuid / driver_uuid are 16 bytes packed as 4×u32 LE on
        // the wire. Unpack back to 16 bytes for the JSON output.
        uint8_t bytes[16] = {0};
        for (uint32_t i = 0; i < a.count && i < 4; ++i) {
            uint32_t v = a.data[i];
            bytes[i*4 + 0] = static_cast<uint8_t>( v        & 0xff);
            bytes[i*4 + 1] = static_cast<uint8_t>((v >>  8) & 0xff);
            bytes[i*4 + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
            bytes[i*4 + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
        }
        std::string s = "[";
        for (int i = 0; i < 16; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%s%u", i ? "," : "", bytes[i]);
            s += buf;
        }
        s += "]";
        return s;
    };

    std::printf("{\n");
    std::printf("  \"by_fourcc\": {\n");
    size_t cursor = 0;
    for (uint32_t i = 0; i < caps.fourccs.count; ++i) {
        const uint32_t fc = caps.fourccs.data[i];
        const uint32_t n  = caps.mod_counts.data[i];
        std::printf("    \"0x%08x\": [", fc);
        for (uint32_t j = 0; j < n; ++j) {
            std::printf("%s\n      {\"modifier\": %llu, \"usage\": %u, \"plane_count\": %u}",
                        j ? "," : "",
                        static_cast<unsigned long long>(caps.modifiers.data[cursor + j]),
                        caps.usages.data[cursor + j],
                        caps.plane_counts.data[cursor + j]);
        }
        cursor += n;
        std::printf("\n    ]%s\n", (i + 1 < caps.fourccs.count) ? "," : "");
    }
    std::printf("  },\n");
    std::printf("  \"device_uuid\": %s,\n", put_uuid(caps.device_uuid).c_str());
    std::printf("  \"driver_uuid\": %s,\n", put_uuid(caps.driver_uuid).c_str());
    std::printf("  \"drm_render_major\": %u,\n", caps.drm_render_major);
    std::printf("  \"drm_render_minor\": %u,\n", caps.drm_render_minor);
    std::printf("  \"sync\": %u,\n", caps.sync_caps);
    std::printf("  \"color\": %u,\n", caps.color_caps);
    std::printf("  \"mem_hint\": %u,\n", caps.mem_hints);
    std::printf("  \"extent_max_w\": %u,\n", caps.extent_max_w);
    std::printf("  \"extent_max_h\": %u\n",  caps.extent_max_h);
    std::printf("}\n");
    std::fflush(stdout);
    ww_evt_format_caps_free(&caps);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);

    if (opt.print_caps) {
        return print_caps_json(opt);
    }

    if (opt.vulkan_probe) {
        std::string verr;
        auto prod = ww_image::VkProducer::create(opt.width, opt.height, &verr);
        if (!prod) {
            std::fprintf(stderr, "waywallen-image-renderer: vk_producer: %s\n",
                         verr.c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "waywallen-image-renderer: vulkan_probe ok "
                     "drm_render=%u:%u\n",
                     prod->drm_render_major(), prod->drm_render_minor());
        return 0;
    }

    if (opt.decode_only) {
        if (opt.image_path.empty()) die("--decode-only requires --image");
        ww_image::DecodeError derr;
        ww_image::RgbaBuf buf =
            ww_image::decode_to_rgba(opt.image_path, opt.width, opt.height, &derr);
        if (buf.data.empty()) {
            std::fprintf(stderr,
                         "waywallen-image-renderer: decode failed: %s\n",
                         derr.message.c_str());
            return 1;
        }
        uint64_t sum = 0;
        for (uint8_t b : buf.data) sum += b;
        std::fprintf(stderr,
                     "waywallen-image-renderer: decoded %ux%u stride=%u "
                     "bytes=%zu pixel_sum=%llu\n",
                     buf.width, buf.height, buf.stride,
                     buf.data.size(),
                     static_cast<unsigned long long>(sum));
        return 0;
    }

    if (opt.ipc_path.empty()) die("--ipc <socket_path> is required");

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* --- Decode + Vulkan setup --- */
    if (opt.image_path.empty()) die("--image is required");
    ww_image::DecodeError derr;
    ww_image::RgbaBuf rgba_buf = ww_image::decode_to_rgba(
        opt.image_path, opt.width, opt.height, &derr);
    if (rgba_buf.data.empty()) die("decode " + opt.image_path + ": " + derr.message);

    std::string verr;
    auto producer = ww_image::VkProducer::create(opt.width, opt.height, &verr);
    if (!producer) die("vk_producer: " + verr);

    /* GPU info diagnostic (uses bridge probe_vk dispatch table). */
    ww_bridge_vk_dt_t vdt {};
    ww_bridge_vk_dt_load(&vdt, vkGetInstanceProcAddr, producer->instance());
    ww_bridge_vk_log_gpu_info("waywallen-image-renderer", &vdt,
                              producer->physical_device());

    HostState host;
    host.sock = ww_bridge_connect(opt.ipc_path.c_str());
    if (host.sock < 0)
        die("ww_bridge_connect: " + std::string(std::strerror(-host.sock)));
    host.rgba_data = rgba_buf.data.data();
    host.rgba_size = rgba_buf.data.size();

    /* --- Bridge pool: hand over Vulkan handles --- */
    ww_pool_vulkan_init_t pool_init {};
    pool_init.instance              = producer->instance();
    pool_init.physical_device       = producer->physical_device();
    pool_init.device                = producer->device();
    pool_init.queue                 = producer->queue();
    pool_init.queue_family_index    = producer->queue_family_index();
    pool_init.get_instance_proc_addr =
        reinterpret_cast<void *(*)(void *, const char *)>(vkGetInstanceProcAddr);
    pool_init.device_uuid           = producer->device_uuid();
    pool_init.driver_uuid           = producer->driver_uuid();
    pool_init.drm_render_major      = producer->drm_render_major();
    pool_init.drm_render_minor      = producer->drm_render_minor();
    pool_init.drm_render_fd         = producer->drm_render_fd();

    if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pool_init, &host.pool);
        rc != 0)
        die("ww_bridge_pool_create failed: " + std::to_string(rc));

    /* Bridge sends ready + release_syncobj + format_caps in one go. */
    if (int rc = ww_bridge_pool_advertise_caps(host.pool, host.sock,
                                               opt.width, opt.height,
                                               WW_MEM_HINT_DEVICE_LOCAL | WW_MEM_HINT_HOST_VISIBLE);
        rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));
    std::fprintf(stderr,
                 "waywallen-image-renderer: ready, advertised caps, "
                 "waiting for NegotiateBuffers\n");

    std::thread reader([&]() { reader_loop(host); });

    /* Main loop: drain pending negotiate requests as they come. Static
     * image: one upload per directive is enough; afterwards we just
     * wait for shutdown. */
    while (!host.shutdown.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(host.neg_mu);
        host.neg_cv.wait(lk, [&] {
            return host.neg_pending
                || host.shutdown.load(std::memory_order_acquire);
        });
        if (host.shutdown.load(std::memory_order_acquire)) break;
        if (host.neg_pending) {
            ww_pool_directive_t d = host.neg_directive;
            host.neg_pending = false;
            lk.unlock();
            apply_negotiate_request(host, *producer, d);
        }
    }

    if (reader.joinable()) {
        ::shutdown(host.sock, SHUT_RD);
        reader.join();
    }
    if (host.pool) ww_bridge_pool_destroy(host.pool);
    ww_bridge_close(host.sock);
    return 0;
}
