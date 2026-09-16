/*
 * fbstream4.c
 * 
 * Bypass CRTC entirely. Use DRM_IOCTL_MODE_GETPLANE to get the active
 * framebuffer id directly from the plane, then GETFB2 → PRIME → mmap.
 *
 * Plane ids we know from debugfs:
 *   plane[126] = plane-10, primary full-screen (1280x2576)
 *   plane[104] = plane-4,  1088x2400
 *   plane[129] = plane-11, 1088x2400
 *
 * Also attempts DRM_IOCTL_MODE_GETPLANE on renderD128 path.
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

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,t) _IOWR(DRM_IOCTL_BASE,(nr),t)
#define DRM_IOW(nr,t)  _IOW( DRM_IOCTL_BASE,(nr),t)
#define DRM_IOR(nr,t)  _IOR( DRM_IOCTL_BASE,(nr),t)

/* ── uapi structs ── */
struct drm_version {
    int version_major, version_minor, version_patchlevel;
    uint64_t name_len; uint64_t name;
    uint64_t date_len; uint64_t date;
    uint64_t desc_len; uint64_t desc;
};
struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
};
/* DRM property value for getting plane fb via object properties */
struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

#define DRM_IOCTL_VERSION            DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_MODE_GETPLANE      DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_GETFB         DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_GETFB2        DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_MODE_OBJECT_PLANE        0xeeeeeeee

static int running = 1;
static void sighandler(int s){ running=0; }
static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}

int try_plane(int fd, uint32_t plane_id,
              uint32_t *out_fb, uint32_t *out_w, uint32_t *out_h,
              uint32_t *out_pitch) {
    /* format_type_ptr must point to valid memory even if count=0 */
    uint32_t fmt_buf[64] = {0};
    struct drm_mode_get_plane p = {0};
    p.plane_id = plane_id;
    p.count_format_types = 0;
    p.format_type_ptr = (uint64_t)(uintptr_t)fmt_buf;

    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) < 0) {
        fprintf(stderr, "  GETPLANE %u: %s\n", plane_id, strerror(errno));
        return -1;
    }

    fprintf(stderr, "  plane %u: crtc=%u fb=%u possible=0x%x\n",
            plane_id, p.crtc_id, p.fb_id, p.possible_crtcs);

    if (!p.fb_id) return -1;

    /* GETFB2 */
    struct drm_mode_fb_cmd2 fb2 = {0};
    fb2.fb_id = p.fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb2) == 0) {
        fprintf(stderr, "    GETFB2: %ux%u fmt=0x%08x mod=0x%llx "
                "pitch=%u handle=%u\n",
                fb2.width, fb2.height, fb2.pixel_format,
                (unsigned long long)fb2.modifier[0],
                fb2.pitches[0], fb2.handles[0]);
        *out_fb    = p.fb_id;
        *out_w     = fb2.width;
        *out_h     = fb2.height;
        *out_pitch = fb2.pitches[0];
        return (fb2.handles[0] || fb2.width) ? 0 : -1;
    }

    /* fallback GETFB (legacy) */
    struct drm_mode_fb_cmd fb1 = {0};
    fb1.fb_id = p.fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb1) == 0) {
        fprintf(stderr, "    GETFB:  %ux%u bpp=%u pitch=%u handle=%u\n",
                fb1.width, fb1.height, fb1.bpp, fb1.pitch, fb1.handle);
        *out_fb    = p.fb_id;
        *out_w     = fb1.width;
        *out_h     = fb1.height;
        *out_pitch = fb1.pitch;
        return fb1.width ? 0 : -1;
    }

    fprintf(stderr, "    GETFB2: %s  GETFB: %s\n",
            strerror(EINVAL), strerror(errno));
    return -1;
}

int main(int argc, char **argv) {
    int port = argc>1 ? atoi(argv[1]) : 5005;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sighandler);

    /* Try both card0 and renderD128 */
    const char *devs[] = {"/dev/dri/card0", "/dev/dri/renderD128"};
    int fd = -1;
    for (int i=0; i<2; i++) {
        fd = open(devs[i], O_RDWR);
        if (fd>=0) { fprintf(stderr,"Opened %s\n", devs[i]); break; }
    }
    if (fd<0){ perror("open drm"); return 1; }

    /* Known active plane ids from debugfs — sorted by priority */
    /* plane-10 (126): full screen 1280x2576 primary */
    /* plane-4  (104): 1088x2400 */
    /* plane-11 (129): 1088x2400 */
    /* plane-3  (101): 1088x240  (status bar) */
    uint32_t plane_ids[] = {126, 104, 129, 116, 110, 113, 119, 123, 101, 0};

    uint32_t best_fb=0, W=0, H=0, pitch=0;
    for (int i=0; plane_ids[i]; i++) {
        uint32_t fb=0, w=0, h=0, p=0;
        if (try_plane(fd, plane_ids[i], &fb, &w, &h, &p) == 0) {
            if (w*h > W*H) { /* pick largest */
                best_fb=fb; W=w; H=h; pitch=p;
            }
        }
    }

    if (!best_fb) {
        fprintf(stderr,"\nNo plane with active FB found.\n");
        fprintf(stderr,"The HWC is likely using atomic commits without "
                "exposing GEM handles to non-owner processes.\n");
        fprintf(stderr,"Trying direct FB ids from debugfs state...\n");

        /* Try known FB ids directly via GETFB2 */
        uint32_t fb_ids[] = {254,253,251,246,248,234,227,172,222,257,255,0};
        for (int i=0; fb_ids[i]; i++) {
            struct drm_mode_fb_cmd2 fb2={0};
            fb2.fb_id = fb_ids[i];
            if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb2)==0) {
                fprintf(stderr,"  FB %u: %ux%u fmt=0x%x mod=0x%llx "
                        "pitch=%u handle=%u\n",
                        fb_ids[i], fb2.width, fb2.height,
                        fb2.pixel_format,
                        (unsigned long long)fb2.modifier[0],
                        fb2.pitches[0], fb2.handles[0]);
                if (fb2.width*fb2.height > W*H) {
                    best_fb=fb_ids[i]; W=fb2.width;
                    H=fb2.height; pitch=fb2.pitches[0];
                }
            } else {
                fprintf(stderr,"  FB %u: %s\n", fb_ids[i], strerror(errno));
            }
        }
    }

    if (!best_fb){ fprintf(stderr,"Cannot find any accessible FB\n"); return 1; }
    if (!pitch) pitch=W*4;
    fprintf(stderr,"\nBest FB: id=%u %ux%u pitch=%u\n", best_fb,W,H,pitch);

    /* GETFB2 for handle */
    struct drm_mode_fb_cmd2 fb2={0};
    fb2.fb_id=best_fb;
    uint32_t gem=0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb2)==0) {
        gem=fb2.handles[0];
        fprintf(stderr,"GEM handle=%u\n", gem);
    }
    if (!gem) {
        struct drm_mode_fb_cmd fb1={0};
        fb1.fb_id=best_fb;
        if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb1)==0) gem=fb1.handle;
        fprintf(stderr,"Legacy GEM handle=%u\n", gem);
    }

    /* PRIME → dma-buf fd */
    int dmabuf_fd=-1;
    if (gem) {
        struct drm_prime_handle ph={.handle=gem,.flags=0,.fd=-1};
        if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)==0) {
            dmabuf_fd=ph.fd;
            fprintf(stderr,"dma-buf fd=%d\n", dmabuf_fd);
        } else {
            fprintf(stderr,"PRIME failed: %s\n", strerror(errno));
        }
    }

    size_t frame_sz=(size_t)pitch*H;
    fprintf(stderr,"Frame: %ux%u pitch=%u sz=%zu\n",W,H,pitch,frame_sz);

    /* mmap */
    void *map=MAP_FAILED;
    if (dmabuf_fd>=0) {
        map=mmap(NULL,frame_sz,PROT_READ,MAP_SHARED,dmabuf_fd,0);
        fprintf(stderr,"mmap: %s\n",
                map==MAP_FAILED?strerror(errno):"OK");
        if (map!=MAP_FAILED) {
            uint8_t *p=map;
            fprintf(stderr,"First 8 pixels:\n");
            for(int i=0;i<8;i++)
                fprintf(stderr,"  [%d] %02x %02x %02x %02x\n",
                        i,p[i*4+0],p[i*4+1],p[i*4+2],p[i*4+3]);
        }
    }

    /* TCP */
    int srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={
        .sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
    bind(srv,(struct sockaddr*)&addr,sizeof(addr));
    listen(srv,1);
    fprintf(stderr,"\nListening :%d\n",port);
    fprintf(stderr,"ffplay -f rawvideo -pixel_format bgra "
            "-video_size %ux%u -framerate 60 "
            "-fflags nobuffer -flags low_delay tcp://PHONE_IP:%d\n\n",W,H,port);

    uint8_t *rowbuf=malloc((pitch>W*4?pitch:W*4)+4);
    while(running){
        int cli=accept(srv,NULL,NULL);
        if(cli<0) break;
        fprintf(stderr,"Client connected\n");
        int nd=1; setsockopt(cli,6,1,&nd,sizeof(nd));
        long long frames=0,t0=now_ms();
        while(running){
            if(map==MAP_FAILED){sleep(1);break;}
            uint8_t *src=(uint8_t*)map;
            int ok=1;
            if(pitch==W*4){
                size_t rem=frame_sz; uint8_t *p=src;
                while(rem>0){
                    ssize_t r=write(cli,p,rem);
                    if(r<=0){ok=0;break;}
                    p+=r;rem-=r;
                }
            } else {
                for(uint32_t row=0;row<H&&ok;row++){
                    memcpy(rowbuf,src+(size_t)row*pitch,W*4);
                    uint8_t *p=rowbuf; size_t rem=W*4;
                    while(rem>0){
                        ssize_t r=write(cli,p,rem);
                        if(r<=0){ok=0;break;}
                        p+=r;rem-=r;
                    }
                }
            }
            if(!ok) break;
            frames++;
            if(frames%120==0)
                fprintf(stderr,"%.1f fps\n",frames*1000.0/(now_ms()-t0));
        }
        fprintf(stderr,"Disconnected frames=%lld\n",frames);
        close(cli);
    }
    free(rowbuf);
    if(map!=MAP_FAILED) munmap(map,frame_sz);
    if(dmabuf_fd>=0) close(dmabuf_fd);
    close(fd);
    return 0;
}