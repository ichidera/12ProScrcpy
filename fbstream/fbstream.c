/*
 * fbstream5.c
 *
 * The UBWC problem: modifier=0x500000000000001 means Qualcomm UBWC tiling.
 * CPU mmap gives us raw tile data, not linear pixels.
 *
 * Solution: use Android's own ScreenCaptureClient via the screencap binary
 * which calls SurfaceFlinger::captureScreen() — this does the GPU-assisted
 * UBWC→linear conversion internally and returns raw RGBA pixels.
 *
 * We call screencap in a tight loop, reading its raw pixel output from a
 * pipe. screencap with no -p flag outputs:
 *   uint32_t width
 *   uint32_t height  
 *   uint32_t format  (RGBA=1, RGBX=2, etc)
 *   uint32_t padding
 *   then raw pixels
 *
 * This bypasses ADB entirely — pure local pipe, then TCP.
 * Latency is limited by SurfaceFlinger capture time (~8-16ms on this hw).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include <signal.h>

static int running = 1;
static void sighandler(int s){ running=0; }
static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}

/* Raw screencap header */
struct sc_header {
    uint32_t width;
    uint32_t height;
    uint32_t format;   /* 1=RGBA_8888, 2=RGBX_8888 */
    uint32_t padding;
};

/* Read exactly n bytes from fd */
static int readn(int fd, void *buf, size_t n) {
    uint8_t *p = buf; size_t rem = n;
    while (rem > 0) {
        ssize_t r = read(fd, p, rem);
        if (r <= 0) return -1;
        p += r; rem -= r;
    }
    return 0;
}

/* Write exactly n bytes to fd */
static int writen(int fd, void *buf, size_t n) {
    uint8_t *p = buf; size_t rem = n;
    while (rem > 0) {
        ssize_t r = write(fd, p, rem);
        if (r <= 0) return -1;
        p += r; rem -= r;
    }
    return 0;
}

/* Capture one frame via screencap, write raw RGBA to out_fd.
   Returns frame size in bytes, or -1 on error. */
static ssize_t capture_frame(int out_fd, uint8_t *framebuf, size_t bufcap,
                              uint32_t *W, uint32_t *H) {
    int pfd[2];
    if (pipe(pfd) < 0) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pfd[0]); close(pfd[1]); return -1; }

    if (pid == 0) {
        /* child: screencap writes to pipe write-end */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        /* screencap without -p → raw binary output */
        execl("/system/bin/screencap", "screencap", NULL);
        _exit(1);
    }

    close(pfd[1]);

    /* Read header */
    struct sc_header hdr;
    if (readn(pfd[0], &hdr, sizeof(hdr)) < 0) {
        waitpid(pid, NULL, 0); close(pfd[0]); return -1;
    }
    *W = hdr.width; *H = hdr.height;
    size_t frame_sz = (size_t)hdr.width * hdr.height * 4;

    if (frame_sz > bufcap) {
        fprintf(stderr, "Frame too large: %zu > %zu\n", frame_sz, bufcap);
        waitpid(pid, NULL, 0); close(pfd[0]); return -1;
    }

    /* Read pixels */
    if (readn(pfd[0], framebuf, frame_sz) < 0) {
        waitpid(pid, NULL, 0); close(pfd[0]); return -1;
    }

    close(pfd[0]);
    waitpid(pid, NULL, 0);
    return (ssize_t)frame_sz;
}

/* Alternative: use screencap via popen (simpler, avoids fork overhead) */
static ssize_t capture_frame_popen(int out_fd, uint8_t *framebuf, size_t bufcap,
                                    uint32_t *W, uint32_t *H) {
    FILE *fp = popen("/system/bin/screencap 2>/dev/null", "r");
    if (!fp) { perror("popen screencap"); return -1; }

    struct sc_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { pclose(fp); return -1; }

    *W = hdr.width; *H = hdr.height;
    size_t frame_sz = (size_t)(*W) * (*H) * 4;

    if (frame_sz > bufcap || frame_sz == 0) {
        fprintf(stderr, "Bad frame: %ux%u sz=%zu\n", *W, *H, frame_sz);
        pclose(fp); return -1;
    }

    if (fread(framebuf, 1, frame_sz, fp) != frame_sz) { pclose(fp); return -1; }
    pclose(fp);
    return (ssize_t)frame_sz;
}

int main(int argc, char **argv) {
    int port = argc>1 ? atoi(argv[1]) : 5005;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Test screencap once to get dimensions */
    fprintf(stderr, "Testing screencap...\n");
    size_t bufcap = 1440*3200*4 + 64;
    uint8_t *framebuf = malloc(bufcap);
    if (!framebuf) { fprintf(stderr,"OOM\n"); return 1; }

    uint32_t W=0, H=0;
    ssize_t fsz = capture_frame_popen(-1, framebuf, bufcap, &W, &H);
    if (fsz < 0) {
        fprintf(stderr,"screencap failed, trying fork method...\n");
        fsz = capture_frame(-1, framebuf, bufcap, &W, &H);
    }
    if (fsz < 0) { fprintf(stderr,"screencap unavailable\n"); free(framebuf); return 1; }

    fprintf(stderr,"Screencap OK: %ux%u = %zd bytes\n", W, H, fsz);
    fprintf(stderr,"First 8 pixels (RGBA):\n");
    for(int i=0;i<8;i++)
        fprintf(stderr,"  [%d] R=%02x G=%02x B=%02x A=%02x\n",
                i, framebuf[i*4], framebuf[i*4+1],
                framebuf[i*4+2], framebuf[i*4+3]);

    /* TCP server */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr={
        .sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
    bind(srv,(struct sockaddr*)&addr,sizeof(addr));
    listen(srv,1);

    fprintf(stderr,"\nListening :%d\n",port);
    fprintf(stderr,"ffplay -f rawvideo -pixel_format rgba "
            "-video_size %ux%u -framerate 30 "
            "-fflags nobuffer -flags low_delay "
            "-framedrop tcp://PHONE_IP:%d\n\n", W, H, port);

    while(running) {
        int cli = accept(srv, NULL, NULL);
        if(cli<0) break;
        fprintf(stderr,"Client connected\n");
        int nd=1; setsockopt(cli,6,1,&nd,sizeof(nd));

        long long frames=0, t0=now_ms(), dropped=0;
        while(running) {
            uint32_t w=0,h=0;
            ssize_t n = capture_frame_popen(-1, framebuf, bufcap, &w, &h);
            if(n<0){ dropped++; continue; }

            if(writen(cli, framebuf, (size_t)n)<0) break;
            frames++;
            if(frames%30==0){
                long long elapsed=now_ms()-t0;
                fprintf(stderr,"%.1f fps (dropped=%lld)\n",
                        frames*1000.0/elapsed, dropped);
            }
        }
        fprintf(stderr,"Disconnected frames=%lld dropped=%lld\n",frames,dropped);
        close(cli);
    }
    free(framebuf);
    return 0;
}