#include "ov_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ov_anchor.h"
#include "ov_state.h"

#define OV_KEY_ENABLED    "overlay.enabled"
#define OV_KEY_INVERT     "overlay.invert"
#define OV_KEY_VOLUME     "overlay.volume.position"
#define OV_KEY_BRIGHTNESS "overlay.brightness.position"
#define OV_KEY_BATTERY    "overlay.battery.position"
#define OV_KEY_NOTIFY     "overlay.notification.position"
#define OV_KEY_FGCOLOR    "overlay.fgcolor"
#define OV_KEY_BGCOLOR    "overlay.bgcolor"
#define OV_KEY_ALPHA      "overlay.alpha"
#define OV_KEY_SCALE      "overlay.scale"
#define OV_KEY_PERCENT    "overlay.battery.percent"
#define OV_KEY_CLOCK      "overlay.clock.show"
#define OV_KEY_CLOCK_POS  "overlay.clock.position"
#define OV_KEY_CLOCK_FMT  "overlay.clock.format"
#define OV_KEY_CLOCK_ALW  "overlay.clock.always"
#define OV_KEY_WIFI_SHOW  "overlay.wifi.show"
#define OV_KEY_BT_SHOW    "overlay.bluetooth.show"
/* Not ours: the switches the rest of the system already keeps here. */
#define OV_KEY_WIFI_ON    "wifi.enabled"
#define OV_KEY_BT_ON      "controllers.bluetooth.enabled"
#define OV_KEY_BEZEL      "overlay.bezel.enabled"
#define OV_KEY_BEZEL_A    "overlay.bezel.alpha"
#define OV_KEY_BEZEL_S    "overlay.bezel.stretch"

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/* "RRGGBB", with an optional leading # or 0x.  Returns 0 on nonsense, leaving
 * the caller's default in place. */
static int parse_colour(const char *v, unsigned *out)
{
    unsigned value = 0;
    int digits = 0;

    if (*v == '#')
        v++;
    else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        v += 2;

    for (; *v; v++, digits++) {
        int d;

        if (*v >= '0' && *v <= '9')
            d = *v - '0';
        else if (*v >= 'a' && *v <= 'f')
            d = *v - 'a' + 10;
        else if (*v >= 'A' && *v <= 'F')
            d = *v - 'A' + 10;
        else
            return 0;
        value = (value << 4) | (unsigned)d;
    }
    if (digits != 6)
        return 0;
    *out = value;
    return 1;
}

/* Accepts either a fraction ("0.9") or a percentage ("90"), because both
 * spellings are natural in this file. */
static int parse_alpha(const char *v, float *out)
{
    char *end;
    float f = strtof(v, &end);

    if (end == v || *end)
        return 0;
    if (f > 1.0f)
        f /= 100.0f;
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    *out = f;
    return 1;
}

/* A plain multiplier: 1.0 leaves the automatic size alone. */
static int parse_scale(const char *v, float *out)
{
    char *end;
    float f = strtof(v, &end);

    if (end == v || *end || f < 0.25f || f > 4.0f)
        return 0;
    *out = f;
    return 1;
}

static int truth(const char *v)
{
    return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") ||
           !strcmp(v, "yes");
}

void ov_config_load(ov_config *cfg)
{
    const char *path = getenv("OV_CONFIG_FILE");
    char line[512];
    FILE *f;

    cfg->enabled = 1;
    cfg->invert = 0;
    cfg->anchor_volume = OV_ANCHOR_DEFAULT_PANEL;
    cfg->anchor_brightness = OV_ANCHOR_DEFAULT_PANEL;
    cfg->anchor_battery = OV_ANCHOR_DEFAULT_BATTERY;
    cfg->anchor_notification = OV_ANCHOR_DEFAULT_NOTIFICATION;
    cfg->fg = OV_COLOUR_UNSET;
    cfg->bg = OV_COLOUR_UNSET;
    cfg->alpha = OV_DEFAULT_ALPHA;
    cfg->scale = OV_DEFAULT_SCALE;
    cfg->battery_percent = 1;
    cfg->clock_show = 0;    /* opt in: it is on screen for the whole game */
    cfg->clock_anchor = OV_ANCHOR_DEFAULT_CLOCK;
    cfg->clock_12h = 0;
    cfg->clock_always = 1;
    cfg->wifi_on = 0;
    cfg->bluetooth_on = 0;
    cfg->wifi_show = 1;
    cfg->bluetooth_show = 1;
    cfg->bezel_enabled = 1;
    cfg->bezel_stretch = 0;
    cfg->bezel_alpha = -1.0f;

    f = fopen((path && *path) ? path : OV_CONFIG_DEFAULT, "r");
    if (!f)
        goto done;

    while (fgets(line, sizeof(line), f)) {
        char *key = trim(line);
        char *val = strchr(key, '=');

        if (!val || *key == '#' || *key == ';')
            continue;
        *val++ = '\0';
        key = trim(key);
        val = trim(val);
        if (!*val)
            continue;               /* an empty value means "leave the default" */

        if (!strcmp(key, OV_KEY_ENABLED))
            cfg->enabled = truth(val);
        else if (!strcmp(key, OV_KEY_INVERT))
            cfg->invert = truth(val);
        else if (!strcmp(key, OV_KEY_VOLUME))
            cfg->anchor_volume = ov_anchor_parse(val, cfg->anchor_volume);
        else if (!strcmp(key, OV_KEY_BRIGHTNESS))
            cfg->anchor_brightness = ov_anchor_parse(val, cfg->anchor_brightness);
        else if (!strcmp(key, OV_KEY_BATTERY))
            cfg->anchor_battery = ov_anchor_parse(val, cfg->anchor_battery);
        else if (!strcmp(key, OV_KEY_NOTIFY))
            cfg->anchor_notification = ov_anchor_parse(val, cfg->anchor_notification);
        else if (!strcmp(key, OV_KEY_FGCOLOR))
            parse_colour(val, &cfg->fg);
        else if (!strcmp(key, OV_KEY_BGCOLOR))
            parse_colour(val, &cfg->bg);
        else if (!strcmp(key, OV_KEY_ALPHA))
            parse_alpha(val, &cfg->alpha);
        else if (!strcmp(key, OV_KEY_SCALE))
            parse_scale(val, &cfg->scale);
        else if (!strcmp(key, OV_KEY_PERCENT))
            cfg->battery_percent = truth(val);
        else if (!strcmp(key, OV_KEY_CLOCK))
            cfg->clock_show = truth(val);
        else if (!strcmp(key, OV_KEY_CLOCK_POS))
            cfg->clock_anchor = ov_anchor_parse(val, cfg->clock_anchor);
        else if (!strcmp(key, OV_KEY_CLOCK_FMT))
            cfg->clock_12h = !strcmp(val, "12");
        else if (!strcmp(key, OV_KEY_CLOCK_ALW))
            cfg->clock_always = truth(val);
        else if (!strcmp(key, OV_KEY_WIFI_ON))
            cfg->wifi_on = truth(val);
        else if (!strcmp(key, OV_KEY_BT_ON))
            cfg->bluetooth_on = truth(val);
        else if (!strcmp(key, OV_KEY_WIFI_SHOW))
            cfg->wifi_show = truth(val);
        else if (!strcmp(key, OV_KEY_BT_SHOW))
            cfg->bluetooth_show = truth(val);
        else if (!strcmp(key, OV_KEY_BEZEL))
            cfg->bezel_enabled = truth(val);
        else if (!strcmp(key, OV_KEY_BEZEL_S))
            cfg->bezel_stretch = truth(val);
        else if (!strcmp(key, OV_KEY_BEZEL_A))
            parse_alpha(val, &cfg->bezel_alpha);
    }
    fclose(f);

done:
    /* An explicit colour wins over overlay.invert, which is just a shorthand
     * for the two colours swapping. */
    if (cfg->fg == OV_COLOUR_UNSET)
        cfg->fg = cfg->invert ? OV_COLOUR_WHITE : OV_COLOUR_BLACK;
    if (cfg->bg == OV_COLOUR_UNSET)
        cfg->bg = cfg->invert ? OV_COLOUR_BLACK : OV_COLOUR_WHITE;
}
