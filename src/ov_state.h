/* Shared overlay state.
 *
 * The state lives in a small file on tmpfs that is mmap'd by both sides:
 * `knulli-overlay` (the writer, run from whatever changes the volume) and the
 * injected library inside the game process (the reader).  Reading is a plain
 * memory load, so the hot path in the swap hook costs nothing when no overlay
 * is due.
 */
#ifndef OV_STATE_H
#define OV_STATE_H

#include <stdint.h>

#define OV_STATE_MAGIC   0x4b4f5636u  /* "KOV6" */

/* Longest notification we carry; anything longer is cut with an ellipsis. */
#define OV_TEXT_MAX 128
#define OV_STATE_DEFAULT "/var/run/knulli-overlay.state"

enum {
    OV_KIND_NONE = 0,
    OV_KIND_VOLUME,
    OV_KIND_BRIGHTNESS
};

enum {
    OV_FLAG_MUTED    = 1u << 0,
    OV_FLAG_CHARGING = 1u << 1,
    /* Radios, carried with the battery: they share its pill and its lifetime. */
    OV_FLAG_WIFI      = 1u << 2,
    OV_FLAG_BLUETOOTH = 1u << 3,
    /* Set to leave the percentage out of the pill; the default (clear) shows
     * it, so a state written by an older CLI still reads correctly. */
    OV_FLAG_NO_PERCENT = 1u << 4
};

#define OV_FLAG_RADIOS (OV_FLAG_WIFI | OV_FLAG_BLUETOOTH)

/* Style bits, resolved from knulli.conf by the writer. */
enum {
    OV_STYLE_INVERT = 1u << 0       /* black card, white ink */
};

/* Colours travel as 0xRRGGBB.  OV_COLOUR_UNSET marks "no explicit value". */
#define OV_COLOUR_UNSET  0xffffffffu
#define OV_COLOUR_BLACK  0x000000u
#define OV_COLOUR_WHITE  0xffffffu
#define OV_DEFAULT_ALPHA 0.92f
#define OV_DEFAULT_SCALE 1.0f

#define OV_BATTERY_NONE (-1)

/* Written with a seqlock: odd `seq` means a write is in flight.
 *
 * The battery has its own value and lifetime, because it rides along with a
 * volume or brightness event as well as being shown on its own. */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t kind;
    int32_t  value;               /* 0..100 */
    uint32_t flags;
    int32_t  battery;             /* 0..100, or OV_BATTERY_NONE */
    uint32_t battery_flags;       /* charging, and which radios are on */
    uint32_t style_flags;
    uint32_t panel_anchor;        /* where this kind of panel goes */
    uint32_t battery_anchor;
    uint32_t fg;                  /* 0xRRGGBB */
    uint32_t bg;
    uint32_t alpha;               /* card opacity, 0..255 */
    uint32_t scale;               /* size multiplier, in thousandths */
    uint32_t notification_anchor;
    uint64_t expire_ms;           /* CLOCK_MONOTONIC ms after which to hide */
    uint64_t battery_expire_ms;
    uint64_t text_expire_ms;
    char     text[OV_TEXT_MAX];   /* notification, NUL terminated */
} ov_state;

/* A consistent copy of the state, as seen by the reader.  `kind` is
 * OV_KIND_NONE and/or `battery` is OV_BATTERY_NONE when that part is not to be
 * drawn -- expiry has already been applied. */
typedef struct {
    uint32_t kind;
    int32_t  value;
    uint32_t flags;
    int32_t  battery;
    uint32_t battery_flags;
    uint32_t style_flags;
    uint32_t panel_anchor;
    uint32_t battery_anchor;
    uint32_t fg;
    uint32_t bg;
    uint32_t alpha;
    uint32_t scale;               /* size multiplier, in thousandths */
    uint32_t notification_anchor;
    char     text[OV_TEXT_MAX];   /* empty when there is nothing to show */
} ov_snapshot;

/* Fills in the defaults a live reader would have seen; for the test programs,
 * which build a snapshot by hand. */
void ov_snapshot_defaults(ov_snapshot *snap);

/* One published event.  A zero duration leaves that element as it was, so the
 * battery can be shown without cutting a volume panel short; a duration of 1ms
 * with OV_KIND_NONE / OV_BATTERY_NONE is how `hide` clears them. */
typedef struct {
    uint32_t kind;
    int32_t  value;
    uint32_t flags;
    uint32_t duration_ms;
    int32_t  battery;
    uint32_t battery_flags;
    uint32_t battery_duration_ms;
    uint32_t style_flags;
    uint32_t panel_anchor;
    uint32_t battery_anchor;
    uint32_t fg;
    uint32_t bg;
    uint32_t alpha;
    uint32_t scale;               /* size multiplier, in thousandths */
    uint32_t notification_anchor;
    uint32_t text_duration_ms;
    const char *text;
} ov_event;

uint64_t ov_now_ms(void);

/* Path of the state file: $OV_STATE_FILE, else OV_STATE_DEFAULT. */
const char *ov_state_path(void);

/* Reader side.  Both calls are safe to make when the file does not exist yet;
 * ov_reader_poll() retries the open at a low rate so a game started before the
 * first volume change still picks the state up later. */
typedef struct {
    const volatile ov_state *map;
    uint64_t next_open_ms;
} ov_reader;

void ov_reader_init(ov_reader *r);
/* Fills `out` and returns 1 if an overlay should be visible right now. */
int ov_reader_poll(ov_reader *r, ov_snapshot *out);

/* Writer side. */
int ov_state_publish(const ov_event *ev);

#endif /* OV_STATE_H */
