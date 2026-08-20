/* The bezel (decoration) drawn behind the status widgets.
 *
 * Emulators that cannot do bezels themselves -- PPSSPP, the standalone pico-8,
 * and everything else the launcher marks as needing an external one -- get
 * theirs from here.  knulli-configgen already writes the choice to
 * /var/run/hud.config before starting the game:
 *
 *     background_image=/usr/share/knulli/.../systems/psp-3_2.png
 *     legacy_layout=false
 *     background_alpha=0
 *
 * so the injected library reads that file itself and nothing has to be wired
 * into the launcher.  The sibling .info file, when there is one, gives the
 * geometry the image was cut for.
 */
#ifndef OV_BEZEL_H
#define OV_BEZEL_H

#include <stdint.h>

#include "ov_png.h"

#define OV_HUD_CONFIG_DEFAULT "/var/run/hud.config"
#define OV_BEZEL_PATH_MAX 512

typedef struct {
    char     path[OV_BEZEL_PATH_MAX];   /* image in use, empty when none */
    ov_image img;
    float    opacity;           /* .info opacity, times any config override */
    int      stretch;           /* fill the screen rather than fit it */
    int      info_w, info_h;    /* geometry the .info describes, 0 when none */
    int      top, left, bottom, right;  /* game window inside that geometry */

    unsigned gen;               /* bumped whenever the image changes */

    /* private */
    int      enabled;
    int      loaded;
    long     src_mtime;
    long     src_size;
    uint64_t next_check_ms;
} ov_bezel;

void ov_bezel_init(ov_bezel *bz);

/* Picks up a new hud.config at most once a second -- cheap enough to call
 * every frame.  Returns 1 when there is an image to draw. */
int  ov_bezel_poll(ov_bezel *bz);

/* Where the image goes on a `sw` x `sh` screen: the whole screen when
 * stretching, otherwise the largest centred rectangle with the image's own
 * aspect ratio. */
void ov_bezel_rect(const ov_bezel *bz, int sw, int sh,
                   float *x, float *y, float *w, float *h);

void ov_bezel_free(ov_bezel *bz);

#endif /* OV_BEZEL_H */
