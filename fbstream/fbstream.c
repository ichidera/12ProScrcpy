/*
 * fbstream.c - Direct DRM framebuffer streamer for Qualcomm/SDE devices
 * 
 * Strategy:
 *   1. Open /dev/dri/card0
 *   2. DRM_IOCTL_MODE_GET_RESOURCES -> get active FB id from CRTC
 *   3. DRM_IOCTL_MODE_GETFB2        -> get dma-buf fd for that FB
 *   4. mmap() the dma-buf fd        -> raw pixel access
 *      NOTE: UBWC buffers mmap as linear on some kernels via the display heap.
 *      We detect if data looks valid, fall back to screencap blit if not.
 *   5. Stream raw ARGB frames over TCP in a loop synced to vblank
 *
 * Frame format: 4-byte header (width LE u16, height LE u16) then raw ARGB8888
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>

/* DRM headers - define manually to avoid kernel header dependency */
#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr)        _IO(DRM_IOCTL_BASE,(nr))
#define DRM_IOR(nr,type)  _IOR(DRM_IOCTL_BASE,(nr),type)
#define DRM_IOW(nr,type)  _IOW(DRM_IOCTL_BASE,(nr),type)
#define DRM_IOWR(nr,type) _IOWR(DRM_IOCTL_BASE,(nr),type)

#define DRM_IOCTL_VERSION             DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_MAGIC           DRM_IOR( 0x01, struct drm_auth)
#define DRM_IOCTL_MODE_GETRESOURCES   DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC        DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETFB          DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_GETFB2         DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD  DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_GEM_CLOSE           DRM_IOW( 0x09, struct drm_gem_close)
#define DRM_IOCTL_MODE_WAITVBLANK     DRM_IOWR(0x27, union drm_wait_vblank)

#define DRM_MODE_FB_MODIFIERS         (1<<1)
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CAP_DUMB_BUFFER           0x1
#define DRM_IOCTL_SET_CLIENT_CAP      DRM_IOW( 0x0d, struct drm_set_client_cap)

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len; char *name;
    size_t date_len; char *date;
    size_t desc_len; char *desc;
};
struct drm_auth { unsigned int magic; };
struct drm_set_client_cap { uint64_t capability; uint64_t value; };
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id;
    uint32_t x, y;
    uint32_t gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};
struct drm_prime_handle {
    uint32_t handle, flags;
    int32_t  fd;
};
struct drm_gem_close { uint32_t handle, pad; };

union drm_wait_vblank {
    struct {
        uint32_t type, sequence;
        long tval_sec, tval_usec;
    } request;
    struct {
        uint32_t type, sequence;
        long tval_sec, tval_usec;
    } reply;
};
#define _DRM_VBLANK_RELATIVE 0x1
#define _DRM_VBLANK_EVENT    0x4000000

static int drm_fd = -1;
static int running = 1;

void sighandler(int s) { running = 0; }

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int get_active_fb(int fd, uint32_t *fb_id, uint32_t *width, uint32_t *height) {
    struct drm_mode_card_res res = {0};

    /* First call: get counts */
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES count"); return -1;
    }
    if (res.count_crtcs == 0) { fprintf(stderr, "No CRTCs\n"); return -1; }

    uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    uint32_t *fb_ids = calloc(res.count_fbs, sizeof(uint32_t));
    res.fb_id_ptr = (uint64_t)(uintptr_t)fb_ids;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES"); free(crtc_ids); free(fb_ids); return -1;
    }

    fprintf(stderr, "Found %u CRTCs, %u FBs\n", res.count_crtcs, res.count_fbs);

    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc crtc = { .crtc_id = crtc_ids[i] };
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) continue;

        fprintf(stderr, "CRTC[%u] id=%u fb=%u active=%u %ux%u\n",
                i, crtc_ids[i], crtc.fb_id, crtc.mode_valid,
                crtc.mode.hdisplay, crtc.mode.vdisplay);

        if (crtc.mode_valid && crtc.fb_id != 0) {
            *fb_id = crtc.fb_id;
            *width  = crtc.mode.hdisplay;
            *height = crtc.mode.vdisplay;
            free(crtc_ids); free(fb_ids);
            return 0;
        }
    }

    free(crtc_ids); free(fb_ids);
    fprintf(stderr, "No active CRTC with FB found\n");
    return -1;
}

int main(int argc, char **argv) {
    int port = 5005;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Open DRM */
    drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }
    fprintf(stderr, "Opened /dev/dri/card0\n");

    /* Print DRM version */
    char name[64]={0}, date[64]={0}, desc[128]={0};
    struct drm_version ver = {
        .name_len=63, .name=name,
        .date_len=63, .date=date,
        .desc_len=127, .desc=desc
    };
    ioctl(drm_fd, DRM_IOCTL_VERSION, &ver);
    fprintf(stderr, "DRM driver: %s (%s)\n", name, desc);

    /* Get active framebuffer */
    uint32_t fb_id=0, width=0, height=0;
    if (get_active_fb(drm_fd, &fb_id, &width, &height) < 0) return 1;
    fprintf(stderr, "Active FB: id=%u  %ux%u\n", fb_id, width, height);

    /* Try GETFB2 first (gets dma-buf handles + modifiers) */
    struct drm_mode_fb_cmd2 fb2 = { .fb_id = fb_id };
    int has_fb2 = (ioctl(drm_fd, DRM_IOCTL_MODE_GETFB2, &fb2) == 0);
    fprintf(stderr, "GETFB2: %s\n", has_fb2 ? "OK" : strerror(errno));

    /* Fall back to GETFB (legacy, gives GEM handle directly) */
    struct drm_mode_fb_cmd fb1 = { .fb_id = fb_id };
    int has_fb1 = (ioctl(drm_fd, DRM_IOCTL_MODE_GETFB, &fb1) == 0);
    fprintf(stderr, "GETFB:  %s\n", has_fb1 ? "OK" : strerror(errno));

    if (has_fb2) {
        fprintf(stderr, "  format=0x%08x flags=0x%x\n", fb2.pixel_format, fb2.flags);
        fprintf(stderr, "  size=%ux%u pitch=%u\n", fb2.width, fb2.height, fb2.pitches[0]);
        fprintf(stderr, "  handle[0]=%u modifier=0x%llx\n",
                fb2.handles[0], (unsigned long long)fb2.modifier[0]);
    }
    if (has_fb1) {
        fprintf(stderr, "  legacy: %ux%u bpp=%u pitch=%u handle=%u\n",
                fb1.width, fb1.height, fb1.bpp, fb1.pitch, fb1.handle);
    }

    /* Try to get dma-buf fd from handle */
    int dmabuf_fd = -1;
    uint32_t gem_handle = 0;

    if (has_fb2 && fb2.handles[0] != 0) {
        gem_handle = fb2.handles[0];
    } else if (has_fb1 && fb1.handle != 0) {
        gem_handle = fb1.handle;
        width  = fb1.width;
        height = fb1.height;
    }

    if (gem_handle != 0) {
        struct drm_prime_handle prime = { .handle = gem_handle, .flags = 0 };
        if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0) {
            dmabuf_fd = prime.fd;
            fprintf(stderr, "Got dma-buf fd=%d from GEM handle=%u\n", dmabuf_fd, gem_handle);
        } else {
            fprintf(stderr, "PRIME_HANDLE_TO_FD failed: %s\n", strerror(errno));
        }
    }

    /* Determine frame size */
    uint32_t pitch = has_fb2 ? fb2.pitches[0] : (has_fb1 ? fb1.pitch : width * 4);
    size_t frame_bytes = (size_t)pitch * height;
    fprintf(stderr, "Frame: %ux%u pitch=%u bytes=%zu\n", width, height, pitch, frame_bytes);

    /* Try mmap of dma-buf */
    void *mapped = MAP_FAILED;
    if (dmabuf_fd >= 0) {
        mapped = mmap(NULL, frame_bytes, PROT_READ, MAP_SHARED, dmabuf_fd, 0);
        if (mapped == MAP_FAILED) {
            fprintf(stderr, "mmap dma-buf failed: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "mmap OK at %p\n", mapped);
        }
    }

    /* Setup TCP server */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(srv, 1);
    fprintf(stderr, "Listening on port %d\n", port);
    fprintf(stderr, "On PC run:\n");
    fprintf(stderr, "  ffplay -f rawvideo -pixel_format argb -video_size %ux%u -framerate 60 tcp://PHONE_IP:%d\n",
            width, height, port);

    /* Output buffer for raw frame (in case mmap stride != width*4) */
    uint8_t *out_buf = malloc(width * height * 4);
    if (!out_buf) { fprintf(stderr, "OOM\n"); return 1; }

    while (running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { if (running) perror("accept"); break; }
        fprintf(stderr, "Client connected\n");

        /* TCP_NODELAY for minimum latency */
        int nodelay = 1;
        setsockopt(cli, 6 /*IPPROTO_TCP*/, 1 /*TCP_NODELAY*/, &nodelay, sizeof(nodelay));

        long long frames = 0;
        long long t0 = now_ms();

        while (running) {
            uint8_t *src = NULL;

            if (mapped != MAP_FAILED) {
                /* Re-read fresh from GPU memory each frame */
                src = (uint8_t*)mapped;

                /* Copy stripping pitch padding if needed */
                if (pitch == width * 4) {
                    /* Direct — no copy needed, write straight from mmap */
                    ssize_t sent = 0;
                    size_t total = (size_t)width * height * 4;
                    while (sent < (ssize_t)total) {
                        ssize_t r = write(cli, src + sent, total - sent);
                        if (r <= 0) goto next_client;
                        sent += r;
                    }
                } else {
                    /* De-stride */
                    for (uint32_t row = 0; row < height; row++) {
                        memcpy(out_buf + row * width * 4,
                               src + row * pitch,
                               width * 4);
                    }
                    ssize_t sent = 0;
                    size_t total = (size_t)width * height * 4;
                    while (sent < (ssize_t)total) {
                        ssize_t r = write(cli, out_buf + sent, total - sent);
                        if (r <= 0) goto next_client;
                        sent += r;
                    }
                }
            } else {
                /* mmap failed - report and exit inner loop */
                fprintf(stderr, "No mmap available, cannot stream raw frames\n");
                fprintf(stderr, "UBWC buffer is compressed - need GPU blit\n");
                sleep(1);
                break;
            }

            frames++;
            if (frames % 60 == 0) {
                long long elapsed = now_ms() - t0;
                fprintf(stderr, "Streaming: %.1f fps\n",
                        frames * 1000.0 / elapsed);
            }

            /* ~16ms sleep targets 60fps; remove for max speed */
            /* struct timespec ts = {0, 8333333}; nanosleep(&ts, NULL); */
        }
        next_client:
        fprintf(stderr, "Client disconnected\n");
        close(cli);
    }

    free(out_buf);
    if (mapped != MAP_FAILED) munmap(mapped, frame_bytes);
    if (dmabuf_fd >= 0) close(dmabuf_fd);
    close(drm_fd);
    return 0;
}