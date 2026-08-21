/* What to draw this frame, independent of the graphics API.
 *
 * The GL hooks and the Vulkan layer both need the same answers -- is anything
 * due, which colours, is there a bezel, what time is it -- so that lives here
 * and each renderer only has to turn an ov_drawlist into draw calls.
 */
#ifndef OV_FRAME_H_INCLUDED
#define OV_FRAME_H_INCLUDED

#include "ov_bezel.h"
#include "ov_layout.h"
#include "ov_state.h"

typedef struct {
    ov_snapshot snap;
    int         have_bezel;
} ov_frame;

/* Reads the environment and knulli.conf.  Safe to call more than once; only
 * the first call does anything. */
void ov_frame_init(void);

/* 0 when the frame can be left alone.  Cheap enough for every frame: a memory
 * load from the shared state, a clock read, and one stat a second. */
int ov_frame_poll(ov_frame *f);

/* Fills `dl` with the bezel (when there is one) and the widgets, laid out for
 * a `w` x `h` screen. */
void ov_frame_build(ov_drawlist *dl, const ov_frame *f, int w, int h);

/* The decoded bezel, for a renderer that has to upload it as a texture. */
const ov_bezel *ov_frame_bezel(void);

int ov_frame_rotation(void);        /* quarter turns clockwise, 0..3 */
int ov_frame_enabled(void);         /* 0 when $OV_DISABLE turned us off */

/* One line to stderr when $OV_DEBUG is set; for things that happen once. */
void ov_log(const char *fmt, ...);

#endif /* OV_FRAME_H_INCLUDED */
