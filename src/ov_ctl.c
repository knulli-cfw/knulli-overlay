/* knulli-overlay: publishes one overlay event for the injected library to
 * pick up.  Meant to be called from whatever changes the value, e.g.
 *
 *     knulli-overlay volume 60
 *     knulli-overlay volume 0 --muted
 *     knulli-overlay brightness 80 --duration 3000
 *     knulli-overlay battery              # battery, wifi and bluetooth
 *     knulli-overlay battery 42 --charging
 *     knulli-overlay notification "Controller connected"
 *     knulli-overlay bezel [W H]           # what the injected library sees
 *     knulli-overlay config                # what knulli.conf resolved to
 *     knulli-overlay hide
 *
 * The battery reading is stamped onto every event, so it comes up alongside
 * the volume and brightness panels without the caller doing anything.  The
 * same goes for the settings out of knulli.conf: they are resolved here, so
 * the injected library never has to touch sysfs or a config file inside a game
 * process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_anchor.h"
#include "ov_battery.h"
#include "ov_bezel.h"
#include "ov_config.h"
#include "ov_state.h"

#define OV_DEFAULT_DURATION_MS 2000
/* Text takes longer to read than a bar takes to glance at. */
#define OV_TEXT_DURATION_MS 3000

static int usage(void)
{
    fprintf(stderr,
            "usage: knulli-overlay volume|brightness <0-100> [--muted] "
            "[--duration MS] [--no-battery]\n"
            "       knulli-overlay battery [0-100] [--charging|--discharging] "
            "[--duration MS]\n"
            "       knulli-overlay status                 # same thing\n"
            "       knulli-overlay notification \"text\" [--duration MS]\n"
            "       knulli-overlay bezel [WIDTH HEIGHT]\n"
            "       knulli-overlay config                 # what knulli.conf resolved to\n"
            "       knulli-overlay hide\n");
    return 2;
}

/* Prints what knulli.conf resolved to -- the quickest answer to "why is that
 * not showing up", since a missing key means a default rather than an error. */
static int show_config(void)
{
    const char *path = getenv("OV_CONFIG_FILE");
    ov_config cfg;

    ov_config_load(&cfg);
    printf("file:        %s\n", (path && *path) ? path : OV_CONFIG_DEFAULT);
    printf("enabled:     %d\n", cfg.enabled);
    printf("colours:     fg %06x  bg %06x  alpha %.2f  invert %d\n",
           cfg.fg, cfg.bg, cfg.alpha, cfg.invert);
    printf("scale:       %.2f\n", cfg.scale);
    printf("volume:      %s\n", ov_anchor_name(cfg.anchor_volume));
    printf("brightness:  %s\n", ov_anchor_name(cfg.anchor_brightness));
    printf("battery:     %s, percent %d\n",
           ov_anchor_name(cfg.anchor_battery), cfg.battery_percent);
    printf("notify:      %s\n", ov_anchor_name(cfg.anchor_notification));
    printf("clock:       show %d, %s, %s, always %d\n", cfg.clock_show,
           ov_anchor_name(cfg.clock_anchor), cfg.clock_12h ? "12h" : "24h",
           cfg.clock_always);
    printf("wifi:        on %d, show %d\n", cfg.wifi_on, cfg.wifi_show);
    printf("bluetooth:   on %d, show %d\n", cfg.bluetooth_on,
           cfg.bluetooth_show);
    printf("bezel:       enabled %d, stretch %d, alpha ", cfg.bezel_enabled,
           cfg.bezel_stretch);
    if (cfg.bezel_alpha < 0.0f)
        printf("from the .info\n");
    else
        printf("%.2f\n", cfg.bezel_alpha);
    return 0;
}

/* Reports the bezel the injected library would draw: the same lookup, run
 * outside a game process where its output can be read. */
static int show_bezel(int argc, char **argv)
{
    ov_bezel bz;
    int w = argc > 2 ? atoi(argv[2]) : 0;
    int h = argc > 3 ? atoi(argv[3]) : 0;

    ov_bezel_init(&bz);
    if (!ov_bezel_poll(&bz)) {
        /* Run from a shell there is normally no bezel to find: it is the
         * game's environment that names one. */
        const char *cfg = getenv("OV_HUD_CONFIG");

        if (!cfg || !*cfg)
            cfg = getenv("MANGOHUD_CONFIGFILE");
        printf("bezel:       none (%s)\n",
               (cfg && *cfg) ? "nothing named in that file"
                             : "MANGOHUD_CONFIGFILE is not set");
        ov_bezel_free(&bz);
        return 0;
    }
    printf("bezel:       %s\n", bz.path);
    printf("image:   %dx%d\n", bz.img.w, bz.img.h);
    printf("opacity: %.2f%s\n", bz.opacity, bz.stretch ? ", stretched" : "");
    if (bz.info_w)
        printf("info:    %dx%d, game window inset t%d l%d b%d r%d\n",
               bz.info_w, bz.info_h, bz.top, bz.left, bz.bottom, bz.right);
    if (w > 0 && h > 0) {
        float x, y, bw, bh;

        ov_bezel_rect(&bz, w, h, &x, &y, &bw, &bh);
        printf("on %dx%d: %.0fx%.0f at %.0f,%.0f\n", w, h, bw, bh, x, y);
    }
    ov_bezel_free(&bz);
    return 0;
}

static long clamp_percent(const char *arg)
{
    long v = strtol(arg, NULL, 10);

    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

int main(int argc, char **argv)
{
    ov_config cfg;
    ov_event ev;
    int battery_only = 0, want_battery = 1, notification = 0;
    int charging = 0, charging_set = 0;
    int have_value = 0;
    int i;

    ov_config_load(&cfg);

    memset(&ev, 0, sizeof(ev));
    ev.duration_ms = OV_DEFAULT_DURATION_MS;
    ev.battery = OV_BATTERY_NONE;
    ev.style_flags = cfg.invert ? OV_STYLE_INVERT : 0;
    ev.battery_anchor = (uint32_t)cfg.anchor_battery;
    ev.notification_anchor = (uint32_t)cfg.anchor_notification;
    ev.fg = cfg.fg;
    ev.bg = cfg.bg;
    ev.alpha = (uint32_t)(cfg.alpha * 255.0f + 0.5f);
    ev.scale = (uint32_t)(cfg.scale * 1000.0f + 0.5f);

    if (argc < 2)
        return usage();

    if (!strcmp(argv[1], "config"))
        return show_config();

    if (!strcmp(argv[1], "bezel"))
        return show_bezel(argc, argv);

    if (!strcmp(argv[1], "hide")) {
        ev.kind = OV_KIND_NONE;
        ev.duration_ms = 1;             /* all three expire immediately */
        ev.battery_duration_ms = 1;
        ev.text_duration_ms = 1;
        ev.text = "";
        return ov_state_publish(&ev) == 0 ? 0 : 1;
    } else if (!strcmp(argv[1], "volume")) {
        ev.kind = OV_KIND_VOLUME;
        ev.panel_anchor = (uint32_t)cfg.anchor_volume;
    } else if (!strcmp(argv[1], "brightness")) {
        ev.kind = OV_KIND_BRIGHTNESS;
        ev.panel_anchor = (uint32_t)cfg.anchor_brightness;
    } else if (!strcmp(argv[1], "battery") || !strcmp(argv[1], "status")) {
        ev.kind = OV_KIND_NONE;
        battery_only = 1;
    } else if (!strcmp(argv[1], "notification")) {
        if (argc < 3)
            return usage();
        ev.kind = OV_KIND_NONE;
        ev.text = argv[2];
        ev.duration_ms = OV_TEXT_DURATION_MS;
        notification = 1;
        want_battery = 0;               /* text is its own event */
    } else {
        return usage();
    }

    for (i = notification ? 3 : 2; i < argc; i++) {
        if (!strcmp(argv[i], "--muted")) {
            ev.flags |= OV_FLAG_MUTED;
        } else if (!strcmp(argv[i], "--charging")) {
            charging = 1;
            charging_set = 1;
        } else if (!strcmp(argv[i], "--discharging")) {
            charging = 0;
            charging_set = 1;
        } else if (!strcmp(argv[i], "--no-battery")) {
            want_battery = 0;
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            ev.duration_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (argv[i][0] != '-' && !have_value) {
            ev.value = (int32_t)clamp_percent(argv[i]);
            have_value = 1;
        } else {
            return usage();
        }
    }

    if (!battery_only && !notification && !have_value)
        return usage();

    /* overlay.enabled=0 turns the whole thing off without touching launchers. */
    if (!cfg.enabled)
        return 0;

    if (want_battery) {
        int sysfs_charging = 0;
        int level = ov_battery_read(&sysfs_charging);
        /* The radios ride in the same pill, so they are read here too -- from
         * the switches the rest of the system already keeps in knulli.conf,
         * which is what the settings UI writes when they are turned on. */
        uint32_t radios = (cfg.wifi_on && cfg.wifi_show ? OV_FLAG_WIFI : 0) |
                          (cfg.bluetooth_on && cfg.bluetooth_show
                               ? OV_FLAG_BLUETOOTH : 0) |
                          (cfg.battery_percent ? 0 : OV_FLAG_NO_PERCENT);

        /* An explicit level on `battery` wins over sysfs, which is what makes
         * the overlay testable on a device with no usable pack. */
        if (battery_only && have_value)
            level = ev.value;
        if (level != OV_BATTERY_UNKNOWN) {
            ev.battery = level;
            ev.battery_flags = radios |
                ((charging_set ? charging : sysfs_charging) ? OV_FLAG_CHARGING : 0);
            ev.battery_duration_ms = ev.duration_ms;
        } else if (radios) {
            /* No usable gauge, but the pill still has something to say. */
            ev.battery = OV_BATTERY_NONE;
            ev.battery_flags = radios;
            ev.battery_duration_ms = ev.duration_ms;
        } else if (battery_only) {
            fprintf(stderr, "knulli-overlay: no battery found\n");
            return 1;
        }
    }

    if (notification) {
        ev.text_duration_ms = ev.duration_ms;
        ev.duration_ms = 0;             /* leave any panel on screen alone */
    } else if (battery_only) {
        ev.duration_ms = 0;
    }

    if (ov_state_publish(&ev) != 0) {
        fprintf(stderr, "knulli-overlay: cannot write %s\n", ov_state_path());
        return 1;
    }
    return 0;
}
