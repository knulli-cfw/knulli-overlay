#include "ov_battery.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSY_DIR "/sys/class/power_supply"

static int read_line(const char *dir, const char *file, char *out, size_t len)
{
    char path[256];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "%s/%s/%s", PSY_DIR, dir, file);
    f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(out, (int)len, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return 1;
}

/* A charger that is online counts as charging even when the battery driver
 * reports "Not charging" -- some of these PMICs do while the pack is full. */
static int mains_online(void)
{
    DIR *d = opendir(PSY_DIR);
    struct dirent *e;
    int online = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char type[32], val[32];

        if (e->d_name[0] == '.')
            continue;
        if (!read_line(e->d_name, "type", type, sizeof(type)))
            continue;
        if (!strcmp(type, "Battery"))
            continue;
        if (read_line(e->d_name, "online", val, sizeof(val)) && val[0] == '1')
            online = 1;
    }
    closedir(d);
    return online;
}

int ov_battery_read(int *charging)
{
    DIR *d = opendir(PSY_DIR);
    struct dirent *e;
    int percent = OV_BATTERY_UNKNOWN;
    int charge = 0;

    if (charging)
        *charging = 0;
    if (!d)
        return OV_BATTERY_UNKNOWN;

    while ((e = readdir(d)) != NULL) {
        char type[32], val[32];

        if (e->d_name[0] == '.')
            continue;
        if (!read_line(e->d_name, "type", type, sizeof(type)) ||
            strcmp(type, "Battery"))
            continue;
        if (read_line(e->d_name, "present", val, sizeof(val)) && val[0] == '0')
            continue;
        if (read_line(e->d_name, "capacity", val, sizeof(val))) {
            int n = atoi(val);

            percent = n < 0 ? 0 : (n > 100 ? 100 : n);
        }
        if (read_line(e->d_name, "status", val, sizeof(val)))
            charge = !strcmp(val, "Charging") || !strcmp(val, "Full");
        if (percent != OV_BATTERY_UNKNOWN)
            break;
    }
    closedir(d);

    if (charging)
        *charging = charge || mains_online();
    return percent;
}
