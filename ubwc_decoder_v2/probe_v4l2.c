/*
 * probe_v4l2.c
 *
 * /sys/class/video4linux/videoN/name came back empty for video32/video33,
 * which just means that sysfs attribute isn't populated -- it doesn't mean
 * the device has no identity. VIDIOC_QUERYCAP asks the driver directly for
 * its name, which is far more reliable. We're looking for anything that
 * looks like a rotator / format-converter / scaler (e.g. "sde_rotator",
 * "c2d", "mdp") as a potential UBWC->linear hardware conversion path that
 * isn't the 3D GPU and isn't the CPU.
 *
 * Build:
 *   aarch64-linux-android30-clang -O2 -o probe_v4l2_arm64 probe_v4l2.c
 * Run:
 *   /data/local/tmp/probe_v4l2_arm64
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

struct v4l2_capability {
    uint8_t  driver[16];
    uint8_t  card[32];
    uint8_t  bus_info[32];
    uint32_t version;
    uint32_t capabilities;
    uint32_t device_caps;
    uint32_t reserved[3];
};

#define VIDIOC_QUERYCAP _IOR('V', 0, struct v4l2_capability)

/* Relevant V4L2_CAP_* bits, from videodev2.h */
#define V4L2_CAP_VIDEO_CAPTURE        0x00000001
#define V4L2_CAP_VIDEO_OUTPUT         0x00000002
#define V4L2_CAP_VIDEO_M2M            0x00008000
#define V4L2_CAP_VIDEO_M2M_MPLANE     0x00004000
#define V4L2_CAP_STREAMING            0x04000000

static void print_caps(uint32_t c){
    fprintf(stderr, "caps=0x%08x [%s%s%s%s%s]\n", c,
        (c & V4L2_CAP_VIDEO_M2M) ? "M2M " : "",
        (c & V4L2_CAP_VIDEO_M2M_MPLANE) ? "M2M_MPLANE " : "",
        (c & V4L2_CAP_VIDEO_CAPTURE) ? "CAPTURE " : "",
        (c & V4L2_CAP_VIDEO_OUTPUT) ? "OUTPUT " : "",
        (c & V4L2_CAP_STREAMING) ? "STREAMING " : "");
}

int main(void){
    for(int i = 0; i < 64; i++){
        char path[64];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if(fd < 0){
            if(errno != ENOENT)
                fprintf(stderr, "%s: open failed: %s (errno=%d)\n",
                        path, strerror(errno), errno);
            continue;
        }

        struct v4l2_capability cap = {0};
        if(ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0){
            fprintf(stderr, "%s: opened but QUERYCAP failed\n", path);
            close(fd);
            continue;
        }

        fprintf(stderr, "%s: driver=\"%.16s\" card=\"%.32s\" bus=\"%.32s\" ",
                path, cap.driver, cap.card, cap.bus_info);
        print_caps(cap.device_caps ? cap.device_caps : cap.capabilities);
        close(fd);
    }
    fprintf(stderr, "\nLooking for anything named like a rotator/scaler/format-"
                     "converter (sde_rotator, c2d, mdp, etc.) with M2M "
                     "capture+output -- that's a candidate for UBWC->linear "
                     "conversion on dedicated hardware.\n");
    return 0;
}
