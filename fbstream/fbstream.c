/*
 * fbstream6.c
 *
 * Strategy: dump raw UBWC bytes from GPU memory as fast as possible.
 * The PC receives and renders them. We also send a small header so the
 * PC knows dimensions. No processing on phone at all.
 *
 * We also add a "pixel probe" mode to test what the buffer actually
 * contains when colorful content is on screen.
 *
 * Header per frame: 4 bytes magic + 4 bytes W + 4 bytes H + 4 bytes flags
 * Then: raw bytes (pitch * height)
 *
 * On PC side we'll write a Python decoder that:
 * 1. First tries to render as-is (maybe it IS linear)
 * 2. If garbage, applies UBWC tile de-interleave
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

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    uint64_t name_len; uint64_t name;
    uint64_t date_len; uint64_t date;
    uint64_t desc_len; uint64_t desc;
};
struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types;
    uint64_t format_type_ptr;
};
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };

#define DRM_IOCTL_VERSION            DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_MODE_GETPLANE      DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_GETFB2        DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)

static int running = 1;
static void sighandler(int s){ running=0; }
static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static int writen(int fd, void *buf, size_t n){
    uint8_t *p=buf; size_t r=n;
    while(r>0){ ssize_t w=write(fd,p,r); if(w<=0) return -1; p+=w; r-=w; }
    return 0;
}

#define MAGIC 0x46425354u  /* "FBST" */

int main(int argc, char **argv){
    int port = argc>1 ? atoi(argv[1]) : 5005;
    int probe_mode = argc>2 && !strcmp(argv[2],"probe");
    signal(SIGPIPE,SIG_IGN); signal(SIGINT,sighandler);

    int fd = open("/dev/dri/card0", O_RDWR);
    if(fd<0){perror("card0");return 1;}

    /* Get plane 126 (full-screen primary) */
    uint32_t plane_ids[]={126,104,129,0};
    uint32_t best_fb=0, W=0, H=0, pitch=0, gem=0;
    uint64_t modifier=0;

    for(int i=0; plane_ids[i]; i++){
        uint32_t fmt[64]={0};
        struct drm_mode_get_plane p={
            .plane_id=plane_ids[i],
            .format_type_ptr=(uint64_t)(uintptr_t)fmt
        };
        if(ioctl(fd,DRM_IOCTL_MODE_GETPLANE,&p)<0) continue;
        if(!p.fb_id) continue;

        struct drm_mode_fb_cmd2 fb={.fb_id=p.fb_id};
        if(ioctl(fd,DRM_IOCTL_MODE_GETFB2,&fb)<0) continue;
        if(fb.width*fb.height > W*H){
            best_fb=p.fb_id; W=fb.width; H=fb.height;
            pitch=fb.pitches[0]; gem=fb.handles[0];
            modifier=fb.modifier[0];
        }
    }

    if(!best_fb){fprintf(stderr,"No FB\n");return 1;}
    if(!pitch) pitch=W*4;
    fprintf(stderr,"FB %u: %ux%u pitch=%u mod=0x%llx gem=%u\n",
            best_fb,W,H,pitch,(unsigned long long)modifier,gem);

    /* PRIME → mmap */
    struct drm_prime_handle ph={.handle=gem,.flags=0,.fd=-1};
    if(ioctl(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&ph)<0){
        perror("PRIME");return 1;
    }
    int dma_fd=ph.fd;
    size_t map_sz=(size_t)pitch*H;

    void *map=mmap(NULL,map_sz,PROT_READ,MAP_SHARED,dma_fd,0);
    if(map==MAP_FAILED){perror("mmap");return 1;}
    fprintf(stderr,"mmap OK: %zu bytes\n",map_sz);

    /* Probe mode: dump 256 bytes at different offsets */
    if(probe_mode){
        uint8_t *p=map;
        size_t offsets[]={0, 4096, 16384, 65536, 
                          map_sz/4, map_sz/2, map_sz*3/4, 0};
        offsets[7]=map_sz-64;
        fprintf(stderr,"\n=== PROBE MODE ===\n");
        for(int i=0;i<8;i++){
            fprintf(stderr,"Offset 0x%zx:\n  ",offsets[i]);
            for(int j=0;j<32;j++)
                fprintf(stderr,"%02x ",p[offsets[i]+j]);
            fprintf(stderr,"\n");
        }
        fprintf(stderr,"=== END PROBE ===\n\n");
    }

    /* TCP server */
    int srv=socket(AF_INET,SOCK_STREAM,0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={
        .sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
    bind(srv,(struct sockaddr*)&addr,sizeof(addr));
    listen(srv,1);

    /* Frame header layout (16 bytes):
     *   u32 magic    = 0x46425354
     *   u32 width
     *   u32 height
     *   u32 pitch
     */
    uint32_t hdr[4]={MAGIC, W, H, pitch};

    fprintf(stderr,"Listening :%d  raw UBWC stream\n",port);
    fprintf(stderr,"Frame: %ux%u pitch=%u = %zu bytes/frame\n",W,H,pitch,map_sz);
    fprintf(stderr,"\nPC decoder cmd:\n");
    fprintf(stderr,"  python3 decode_ubwc.py --host PHONE_IP --port %d "
            "--width %u --height %u --pitch %u\n\n",port,W,H,pitch);

    while(running){
        int cli=accept(srv,NULL,NULL);
        if(cli<0) break;
        fprintf(stderr,"Client connected\n");
        int nd=1; setsockopt(cli,6,1,&nd,sizeof(nd));

        long long frames=0,t0=now_ms();
        while(running){
            /* Send header */
            if(writen(cli,hdr,sizeof(hdr))<0) break;
            /* Send raw frame — zero processing, maximum speed */
            if(writen(cli,(void*)map,map_sz)<0) break;
            frames++;
            if(frames%60==0)
                fprintf(stderr,"%.1f fps  %.1f MB/s\n",
                        frames*1000.0/(now_ms()-t0),
                        (double)frames*map_sz/1e6*1000.0/(now_ms()-t0));
        }
        fprintf(stderr,"Disconnected frames=%lld\n",frames);
        close(cli);
    }
    munmap(map,map_sz); close(dma_fd); close(fd);
    return 0;
}