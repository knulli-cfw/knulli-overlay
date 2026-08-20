/* Reads the overlay's settings out of knulli.conf.
 *
 * Only the CLI does this: it resolves the configuration and stamps the result
 * into the shared state, so a game process never opens a config file.  The
 * file is the plain key=value one that knulli-settings-get reads, and the
 * values here are exactly what
 *
 *     knulli-settings-get overlay.battery.position
 *
 * prints -- parsed directly rather than through the tool, because the volume
 * keys repeat at 10Hz while held and forking per key press is not free on
 * these SoCs.
 */
#ifndef OV_CONFIG_H
#define OV_CONFIG_H

#define OV_CONFIG_DEFAULT "/userdata/system/knulli.conf"

typedef struct {
    int enabled;
    int invert;             /* black card, white ink */
    int anchor_volume;
    int anchor_brightness;
    int anchor_battery;
    int anchor_notification;
    unsigned fg;            /* 0xRRGGBB: bars, icons and text */
    unsigned bg;            /* 0xRRGGBB: the card behind them */
    float alpha;            /* card opacity, 0..1 */
    float scale;            /* size multiplier, 1.0 = the built-in size */
    int   battery_percent;  /* draw the numeric level beside the battery */
    /* The clock is drawn by the library itself: it has no event to ride on,
     * and the time has to be current in the frame being drawn. */
    int   clock_show;
    int   clock_anchor;
    int   clock_12h;
    int   clock_always;     /* 0: only while the panel or pill is up */
    /* Radios.  `wifi_on`/`bluetooth_on` come from the keys the rest of the
     * system already sets; the `*_show` pair is the overlay's own switch. */
    int   wifi_on;
    int   bluetooth_on;
    int   wifi_show;
    int   bluetooth_show;
    /* Bezels (see ov_bezel.h).  They are independent of `enabled`: a device
     * that wants decorations but no status widgets is a reasonable setup. */
    int   bezel_enabled;
    int   bezel_stretch;    /* fill the screen instead of keeping the aspect */
    float bezel_alpha;      /* 0..1, or -1 to use the .info's own opacity */
} ov_config;

/* Never fails: a missing or unreadable file just leaves the defaults in place.
 * $OV_CONFIG_FILE overrides the path. */
void ov_config_load(ov_config *cfg);

#endif /* OV_CONFIG_H */
