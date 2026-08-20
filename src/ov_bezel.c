#include "ov_bezel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ov_config.h"
#include "ov_state.h"

/* hud.config is written just before the game starts, and the process that
 * reads it may have been running since before then (EmulationStation keeps the
 * same process across launches on some setups), so it is re-read now and
 * again rather than only at startup. */
#define OV_BEZEL_RECHECK_MS 1000

static const char *hud_config_path(void)
{
    const char *p = getenv("OV_HUD_CONFIG");

    if (p && *p)
        return p;
    /* configgen points MangoHud at the same file through the environment. */
    p = getenv("MANGOHUD_CONFIGFILE");
    return (p && *p) ? p : OV_HUD_CONFIG_DEFAULT;
}

static void trim(char *s)
{
    size_t n = strlen(s);

    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* Reads background_image= out of hud.config.  Returns 0 when the file names
 * no bezel, which is the normal case for an emulator that draws its own. */
static int read_hud_config(const char *path, char *out, size_t out_len)
{
    FILE *f = fopen(path, "r");
    char line[OV_BEZEL_PATH_MAX];   /* a longer line cannot hold a usable path */
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *v;

        trim(line);
        if (strncmp(line, "background_image=", 17) != 0)
            continue;
        v = line + 17;
        if (!*v || !strcmp(v, "none"))
            break;
        snprintf(out, out_len, "%s", v);
        found = 1;
    }
    fclose(f);
    return found;
}

/* The .info file is JSON, but only ever the flat object batocera writes, so a
 * scan for "key": <number> is enough and saves carrying a parser. */
static int info_number(const char *text, const char *key, float *out)
{
    char pattern[32];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(text, pattern);
    if (!p)
        return 0;
    p = strchr(p, ':');
    if (!p)
        return 0;
    *out = (float)atof(p + 1);
    return 1;
}

static void read_info(ov_bezel *bz)
{
    char path[OV_BEZEL_PATH_MAX + 8];
    char text[1024];
    FILE *f;
    size_t n;
    char *dot;
    float v;

    bz->info_w = bz->info_h = 0;
    bz->top = bz->left = bz->bottom = bz->right = 0;
    bz->opacity = 1.0f;

    snprintf(path, sizeof(path), "%s", bz->path);
    dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/'))
        return;
    /* A resized bezel (configgen writes /tmp/bezel.png) has no .info; the
     * image's own size is then all there is, which is all we need. */
    snprintf(dot, sizeof(path) - (size_t)(dot - path), ".info");
    f = fopen(path, "r");
    if (!f)
        return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';

    if (info_number(text, "width", &v))   bz->info_w = (int)v;
    if (info_number(text, "height", &v))  bz->info_h = (int)v;
    if (info_number(text, "top", &v))     bz->top = (int)v;
    if (info_number(text, "left", &v))    bz->left = (int)v;
    if (info_number(text, "bottom", &v))  bz->bottom = (int)v;
    if (info_number(text, "right", &v))   bz->right = (int)v;
    if (info_number(text, "opacity", &v) && v > 0.0f && v <= 1.0f)
        bz->opacity = v;
}

void ov_bezel_init(ov_bezel *bz)
{
    ov_config cfg;
    const char *env;

    memset(bz, 0, sizeof(*bz));
    bz->opacity = 1.0f;

    ov_config_load(&cfg);
    bz->enabled = cfg.bezel_enabled;
    bz->stretch = cfg.bezel_stretch;
    /* Remembered here and applied after the .info's own opacity. */
    bz->img.w = bz->img.h = 0;

    env = getenv("OV_BEZEL_DISABLE");
    if (env && atoi(env) != 0)
        bz->enabled = 0;
}

static void load(ov_bezel *bz, const char *path)
{
    ov_config cfg;
    struct stat st;

    ov_image_free(&bz->img);
    snprintf(bz->path, sizeof(bz->path), "%s", path);
    read_info(bz);

    ov_config_load(&cfg);
    bz->stretch = cfg.bezel_stretch;
    if (cfg.bezel_alpha >= 0.0f)
        bz->opacity = cfg.bezel_alpha;

    if (ov_png_load(bz->path, &bz->img) != 0) {
        bz->path[0] = '\0';
        bz->loaded = 0;
        return;
    }
    if (stat(bz->path, &st) == 0) {
        bz->src_mtime = (long)st.st_mtime;
        bz->src_size = (long)st.st_size;
    }
    bz->loaded = 1;
    bz->gen++;
    fprintf(stderr, "knulli-overlay: bezel %s (%dx%d)\n", bz->path,
            bz->img.w, bz->img.h);
}

static void unload(ov_bezel *bz)
{
    if (!bz->loaded && !bz->path[0])
        return;
    ov_image_free(&bz->img);
    bz->path[0] = '\0';
    bz->loaded = 0;
    bz->gen++;
}

int ov_bezel_poll(ov_bezel *bz)
{
    char want[OV_BEZEL_PATH_MAX];
    const char *env;
    uint64_t now;
    struct stat st;

    if (!bz->enabled)
        return 0;

    now = ov_now_ms();
    if (bz->next_check_ms && now < bz->next_check_ms)
        return bz->loaded;
    bz->next_check_ms = now + OV_BEZEL_RECHECK_MS;

    want[0] = '\0';
    env = getenv("OV_BEZEL");           /* testing override */
    if (env && *env) {
        if (!strcmp(env, "none")) {
            unload(bz);
            return 0;
        }
        snprintf(want, sizeof(want), "%s", env);
    } else if (!read_hud_config(hud_config_path(), want, sizeof(want))) {
        unload(bz);
        return 0;
    }

    if (bz->loaded && !strcmp(want, bz->path)) {
        /* Same name: only reload if the file behind it changed, which is what
         * happens when configgen rewrites its resized /tmp/bezel.png. */
        if (stat(bz->path, &st) == 0 &&
            ((long)st.st_mtime != bz->src_mtime ||
             (long)st.st_size != bz->src_size))
            load(bz, want);
        return bz->loaded;
    }

    load(bz, want);
    return bz->loaded;
}

void ov_bezel_rect(const ov_bezel *bz, int sw, int sh,
                   float *x, float *y, float *w, float *h)
{
    float iw = (float)bz->img.w, ih = (float)bz->img.h;
    float rw, rh;

    *x = *y = 0.0f;
    *w = (float)sw;
    *h = (float)sh;
    if (bz->stretch || iw <= 0.0f || ih <= 0.0f)
        return;

    /* Configgen normally hands us an image already cut to the screen, so this
     * only bites on a mismatch: keep the aspect ratio and centre it, the same
     * thing its own resize does rather than distorting the artwork. */
    rw = (float)sw;
    rh = rw * ih / iw;
    if (rh > (float)sh) {
        rh = (float)sh;
        rw = rh * iw / ih;
    }
    *w = rw;
    *h = rh;
    *x = ((float)sw - rw) * 0.5f;
    *y = ((float)sh - rh) * 0.5f;
}

void ov_bezel_free(ov_bezel *bz)
{
    ov_image_free(&bz->img);
    bz->path[0] = '\0';
    bz->loaded = 0;
}
