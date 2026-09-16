/*
 * probe_writeback.c
 *
 * Checks whether this DRM node exposes a WRITEBACK connector, which would
 * let us capture already-decompressed (UBWC -> linear) composited frames
 * using dedicated display hardware instead of the 3D GPU or the CPU.
 *
 * This does NOT capture anything yet -- it's a feasibility probe. If it
 * finds a writeback connector with WRITEBACK_PIXEL_FORMATS /
 * WRITEBACK_FB_ID / WRITEBACK_OUT_FENCE_PTR properties, that confirms the
 * kernel side supports the near-zero-load capture path and it's worth
 * building. If it finds nothing, we know to fall back to something else
 * (keep decoding UBWC on the PC, or accept a GPU blit despite the cost).
 *
 * Build (same toolchain you used for fbstream6_arm64):
 *   aarch64-linux-android30-clang -O2 -o probe_writeback_arm64 probe_writeback.c
 *
 * Run on device:
 *   /data/local/tmp/probe_writeback_arm64
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,t) _IOWR(DRM_IOCTL_BASE,(nr),t)

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel;
    uint32_t pad;
};

#define DRM_PROP_NAME_LEN 32
struct drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values, count_enum_blobs;
};

#define DRM_IOCTL_MODE_GETRESOURCES  DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPROPERTY   DRM_IOWR(0xAA, struct drm_mode_get_property)

#define DRM_MODE_CONNECTOR_WRITEBACK 18

static const char *conn_type_name(uint32_t t){
    switch(t){
        case 0: return "Unknown";
        case 11: return "HDMI-A";
        case 14: return "eDP";
        case 15: return "VIRTUAL";
        case 16: return "DSI";
        case 17: return "DPI";
        case 18: return "WRITEBACK";
        default: return "other";
    }
}

int main(void){
    int fd = open("/dev/dri/card0", O_RDWR);
    if(fd < 0){ perror("card0"); return 1; }

    struct drm_mode_card_res res = {0};
    if(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0){
        perror("GETRESOURCES (pass 1)"); return 1;
    }

    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    /* zero the other list pointers so the kernel doesn't try to write
     * counts we don't care about into unallocated buffers */
    res.fb_id_ptr = res.crtc_id_ptr = res.encoder_id_ptr = 0;
    if(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0){
        perror("GETRESOURCES (pass 2)"); return 1;
    }

    fprintf(stderr, "Found %u connectors on card0\n\n", res.count_connectors);

    int found_writeback = 0;

    for(uint32_t i = 0; i < res.count_connectors; i++){
        struct drm_mode_get_connector c = {0};
        c.connector_id = conn_ids[i];
        if(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0){
            fprintf(stderr, "  connector %u: GETCONNECTOR failed\n", conn_ids[i]);
            continue;
        }

        fprintf(stderr, "Connector id=%u type=%s(%u) connection=%u props=%u\n",
                c.connector_id, conn_type_name(c.connector_type),
                c.connector_type, c.connection, c.count_props);

        if(c.connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
            continue;

        found_writeback = 1;

        /* Second pass: fetch this connector's property ids/values so we can
         * confirm the WRITEBACK_* properties exist. */
        uint32_t nprops = c.count_props;
        uint32_t *prop_ids = calloc(nprops, sizeof(uint32_t));
        uint64_t *prop_vals = calloc(nprops, sizeof(uint64_t));
        struct drm_mode_get_connector c2 = {0};
        c2.connector_id = c.connector_id;
        c2.props_ptr = (uint64_t)(uintptr_t)prop_ids;
        c2.prop_values_ptr = (uint64_t)(uintptr_t)prop_vals;
        c2.count_props = nprops;
        if(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2) < 0){
            perror("  GETCONNECTOR (props)");
        } else {
            fprintf(stderr, "  -> properties:\n");
            for(uint32_t p = 0; p < nprops; p++){
                struct drm_mode_get_property gp = {0};
                gp.prop_id = prop_ids[p];
                if(ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0) continue;
                fprintf(stderr, "     %-28s = %llu\n", gp.name,
                        (unsigned long long)prop_vals[p]);
            }
        }
        free(prop_ids); free(prop_vals);
        fprintf(stderr, "\n");
    }

    free(conn_ids);
    close(fd);

    if(found_writeback){
        fprintf(stderr, "RESULT: writeback connector present. "
                "Look above for WRITEBACK_PIXEL_FORMATS / WRITEBACK_FB_ID / "
                "WRITEBACK_OUT_FENCE_PTR -- if those three are listed, the "
                "hardware-writeback capture path is viable.\n");
    } else {
        fprintf(stderr, "RESULT: no writeback connector found on this node. "
                "The near-zero-load hardware capture path isn't available "
                "here -- we'll need a different strategy.\n");
    }
    return 0;
}
