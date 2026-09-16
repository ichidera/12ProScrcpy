
/*
 * fbstream2.c - Fixed struct layout for Android 64-bit DRM ioctls
 * 
 * The issue: drm_mode_card_res uses __u64 for pointer fields.
 * We must pass actual kernel-ABI structs, not our own definitions.
 * Using the exact Android kernel uapi layout.
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
#include <netinet/in.h>
#include <time.h>
#include <signal.h>

/* ── exact Android kernel uapi/drm/drm.h & drm_mode.h layouts ── */

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,t) _IOWR(DRM_IOCTL_BASE,(nr),t)
#define DRM_IOW(nr,t)  _IOW( DRM_IOCTL_BASE,(nr),t)
#define DRM_IOR(nr,t)  _IOR( DRM_IOCTL_BASE,(nr),t)

/* drm.h */
struct drm_version {
    int version_major, version_minor, version_patchlevel;
    uint64_t name_len; uint64_t name;
    uint64_t date_len; uint64_t date;
    uint64_t desc_len; uint64_t desc;
};
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
};

/* drm_mode.h */
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width,  max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[32];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
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

#define DRM_IOCTL_VERSION           DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETFB        DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_GETFB2       DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)

/* ── helpers ── */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static int running = 1;
static void sighandler(int s){ running=0; }

/* ── main ── */
int main(int argc, char **argv) {
    int port = argc>1 ? atoi(argv[1]) : 5005;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  sighandler);

    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd<0){ perror("open card0"); return 1; }
    fprintf(stderr,"Opened /dev/dri/card0\n");

    /* version */
    char dname[64]={0};
    struct drm_version ver={0};
    ver.name_len=63; ver.name=(uint64_t)(uintptr_t)dname;
    ioctl(fd, DRM_IOCTL_VERSION, &ver);
    fprintf(stderr,"Driver: %s\n", dname);

    /* ── GETRESOURCES: two-phase (count then fetch) ── */
    struct drm_mode_card_res res={0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)<0){
        perror("GETRESOURCES phase1"); return 1;
    }
    fprintf(stderr,"Resources: fbs=%u crtcs=%u connectors=%u encoders=%u\n",
            res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders);

    uint32_t ncrtcs = res.count_crtcs;
    uint32_t nfbs   = res.count_fbs;
    uint32_t *crtc_ids = calloc(ncrtcs, sizeof(uint32_t));
    uint32_t *fb_ids   = calloc(nfbs?nfbs:1, sizeof(uint32_t));

    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids;
    res.fb_id_ptr        = (uint64_t)(uintptr_t)fb_ids;
    res.connector_id_ptr = 0;
    res.encoder_id_ptr   = 0;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)<0){
        perror("GETRESOURCES phase2"); return 1;
    }

    fprintf(stderr,"CRTC ids:");
    for(uint32_t i=0;i<ncrtcs;i++) fprintf(stderr," %u", crtc_ids[i]);
    fprintf(stderr,"\n");

    /* ── Find active CRTC ── */
    uint32_t active_fb=0, W=0, H=0;
    for(uint32_t i=0;i<ncrtcs;i++){
        struct drm_mode_crtc crtc={0};
        crtc.crtc_id = crtc_ids[i];
        if(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc)<0){
            fprintf(stderr,"GETCRTC %u failed: %s\n", crtc_ids[i], strerror(errno));
            continue;
        }
        fprintf(stderr,"CRTC %u: fb=%u mode_valid=%u %ux%u\n",
                crtc_ids[i], crtc.fb_id, crtc.mode_valid,
                crtc.mode.hdisplay, crtc.mode.vdisplay);
        if(crtc.mode_valid && crtc.fb_id){
            active_fb = crtc.fb_id;
            W = crtc.mode.hdisplay;
            H = crtc.mode.vdisplay;
        }
    }
    free(crtc_ids); free(fb_ids);

    if(!active_fb){ fprintf(stderr,"No active FB found\n"); return 1; }
    fprintf(stderr,"Active FB id=%u  %ux%u\n", active_fb, W, H);

    /* ── GETFB2 ── */
    struct drm_mode_fb_cmd2 fb2={0};
    fb2.fb_id = active_fb;
    if(ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb2)==0){
        fprintf(stderr,"GETFB2 OK: %ux%u fmt=0x%08x mod=0x%llx pitch=%u handle=%u\n",
                fb2.width, fb2.height, fb2.pixel_format,
                (unsigned long long)fb2.modifier[0],
                fb2.pitches[0], fb2.handles[0]);
    } else {
        fprintf(stderr,"GETFB2 failed: %s\n", strerror(errno));
    }

    /* ── GETFB (legacy) ── */
    struct drm_mode_fb_cmd fb1={0};
    fb1.fb_id = active_fb;
    if(ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb1)==0){
        fprintf(stderr,"GETFB  OK: %ux%u bpp=%u pitch=%u handle=%u\n",
                fb1.width, fb1.height, fb1.bpp, fb1.pitch, fb1.handle);
    } else {
        fprintf(stderr,"GETFB  failed: %s\n", strerror(errno));
    }

    /* ── PRIME: GEM handle → dma-buf fd ── */
    uint32_t gem = fb2.handles[0] ? fb2.handles[0] : fb1.handle;
    uint32_t pitch = fb2.pitches[0] ? fb2.pitches[0] : fb1.pitch;
    if(!W){ W=fb2.width?fb2.width:fb1.width; }
    if(!H){ H=fb2.height?fb2.height:fb1.height; }
    if(!pitch) pitch = W*4;

    fprintf(stderr,"GEM handle=%u  pitch=%u\n", gem, pitch);

    int dmabuf_fd = -1;
    if(gem){
        struct drm_prime_handle ph={0};
        ph.handle = gem;
        ph.flags  = 0;
        if(ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)==0){
            dmabuf_fd = ph.fd;
            fprintf(stderr,"dma-buf fd=%d\n", dmabuf_fd);
        } else {
            fprintf(stderr,"PRIME failed: %s\n", strerror(errno));
        }
    }

    size_t frame_sz = (size_t)pitch * H;
    fprintf(stderr,"Frame size: %zu bytes  (%ux%u pitch=%u)\n", frame_sz, W, H, pitch);

    /* ── mmap ── */
    void *map = MAP_FAILED;
    if(dmabuf_fd>=0){
        map = mmap(NULL, frame_sz, PROT_READ, MAP_SHARED, dmabuf_fd, 0);
        fprintf(stderr,"mmap: %s\n", map==MAP_FAILED ? strerror(errno) : "OK");
    }

    /* probe first 64 bytes */
    if(map != MAP_FAILED){
        uint8_t *p = map;
        fprintf(stderr,"First 16 pixels (ARGB):\n");
        for(int i=0;i<16;i++){
            fprintf(stderr,"  [%2d] A=%02x R=%02x G=%02x B=%02x\n",
                    i, p[i*4+3], p[i*4+2], p[i*4+1], p[i*4+0]);
        }
    }

    /* ── TCP server ── */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr={
        .sin_family=AF_INET, .sin_port=htons(port), .sin_addr.s_addr=INADDR_ANY
    };
    bind(srv,(struct sockaddr*)&addr,sizeof(addr));
    listen(srv,1);

    fprintf(stderr,"\nListening on :%d\n", port);
    fprintf(stderr,"PC cmd: ffplay -f rawvideo -pixel_format bgra "
                   "-video_size %ux%u -framerate 60 tcp://PHONE_IP:%d\n\n", W, H, port);

    uint8_t *rowbuf = malloc(W*4);

    while(running){
        int cli = accept(srv, NULL, NULL);
        if(cli<0) break;
        fprintf(stderr,"Client connected\n");
        int nd=1; setsockopt(cli,6,1,&nd,sizeof(nd));

        long long frames=0, t0=now_ms();
        while(running){
            if(map==MAP_FAILED){ sleep(1); break; }

            uint8_t *src = (uint8_t*)map;
            int ok=1;

            if(pitch == W*4){
                /* zero-copy path */
                size_t rem = frame_sz;
                uint8_t *p = src;
                while(rem>0){
                    ssize_t r = write(cli, p, rem);
                    if(r<=0){ ok=0; break; }
                    p+=r; rem-=r;
                }
            } else {
                /* de-stride */
                for(uint32_t row=0; row<H && ok; row++){
                    memcpy(rowbuf, src+row*pitch, W*4);
                    uint8_t *p=rowbuf; size_t rem=W*4;
                    while(rem>0){
                        ssize_t r=write(cli,p,rem);
                        if(r<=0){ok=0;break;}
                        p+=r; rem-=r;
                    }
                }
            }
            if(!ok) break;

            frames++;
            if(frames%120==0){
                fprintf(stderr,"%.1f fps\n", frames*1000.0/(now_ms()-t0));
            }
        }
        fprintf(stderr,"Client disconnected (frames=%lld)\n", frames);
        close(cli);
    }

    free(rowbuf);
    if(map!=MAP_FAILED) munmap(map,frame_sz);
    if(dmabuf_fd>=0) close(dmabuf_fd);
    close(fd);
    return 0;
}