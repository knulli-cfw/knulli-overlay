#include "ov_state.h"

#include "ov_anchor.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* How long to wait before retrying to open a state file that is not there. */
#define OV_REOPEN_INTERVAL_MS 1000

uint64_t ov_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

const char *ov_state_path(void)
{
    const char *env = getenv("OV_STATE_FILE");
    return (env && *env) ? env : OV_STATE_DEFAULT;
}

static void *ov_map(int prot, int create)
{
    int flags = create ? (O_RDWR | O_CREAT) : O_RDONLY;
    int fd = open(ov_state_path(), flags | O_CLOEXEC, 0666);
    void *map;

    if (fd < 0)
        return NULL;
    if (create && ftruncate(fd, sizeof(ov_state)) != 0) {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, sizeof(ov_state), prot, MAP_SHARED, fd, 0);
    close(fd);
    return (map == MAP_FAILED) ? NULL : map;
}

void ov_reader_init(ov_reader *r)
{
    memset(r, 0, sizeof(*r));
}

int ov_reader_poll(ov_reader *r, ov_snapshot *out)
{
    const volatile ov_state *s = r->map;
    uint64_t now = ov_now_ms();
    uint32_t seq0, seq1;
    uint64_t expire, battery_expire, text_expire;
    int tries;

    if (!s) {
        if (now < r->next_open_ms)
            return 0;
        r->next_open_ms = now + OV_REOPEN_INTERVAL_MS;
        s = r->map = ov_map(PROT_READ, 0);
        if (!s)
            return 0;
    }

    /* seqlock read: retry while the writer is mid-update */
    for (tries = 0; tries < 4; tries++) {
        seq0 = s->seq;
        if (seq0 & 1u)
            continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        out->kind = s->kind;
        out->value = s->value;
        out->flags = s->flags;
        out->battery = s->battery;
        out->battery_flags = s->battery_flags;
        out->style_flags = s->style_flags;
        out->panel_anchor = s->panel_anchor;
        out->battery_anchor = s->battery_anchor;
        out->fg = s->fg;
        out->bg = s->bg;
        out->alpha = s->alpha;
        out->scale = s->scale;
        out->notification_anchor = s->notification_anchor;
        memcpy(out->text, (const void *)s->text, sizeof(out->text));
        out->text[sizeof(out->text) - 1] = '\0';
        expire = s->expire_ms;
        battery_expire = s->battery_expire_ms;
        text_expire = s->text_expire_ms;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        seq1 = s->seq;
        if (seq0 != seq1 || s->magic != OV_STATE_MAGIC)
            continue;

        if (now >= expire)
            out->kind = OV_KIND_NONE;
        if (now >= battery_expire) {
            out->battery = OV_BATTERY_NONE;
            out->battery_flags = 0;     /* the radio icons go with it */
        }
        if (now >= text_expire)
            out->text[0] = '\0';
        /* A device with no usable gauge still shows the pill for its radios. */
        return out->kind != OV_KIND_NONE || out->battery != OV_BATTERY_NONE ||
               (out->battery_flags & OV_FLAG_RADIOS) || out->text[0] != '\0';
    }
    return 0;
}

void ov_snapshot_defaults(ov_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->battery = OV_BATTERY_NONE;
    snap->panel_anchor = OV_ANCHOR_DEFAULT_PANEL;
    snap->battery_anchor = OV_ANCHOR_DEFAULT_BATTERY;
    snap->fg = OV_COLOUR_BLACK;
    snap->bg = OV_COLOUR_WHITE;
    snap->alpha = (uint32_t)(OV_DEFAULT_ALPHA * 255.0f + 0.5f);
    snap->scale = (uint32_t)(OV_DEFAULT_SCALE * 1000.0f + 0.5f);
    snap->notification_anchor = OV_ANCHOR_DEFAULT_NOTIFICATION;
}

int ov_state_publish(const ov_event *ev)
{
    volatile ov_state *s = ov_map(PROT_READ | PROT_WRITE, 1);
    uint64_t now = ov_now_ms();

    if (!s)
        return -1;

    s->seq++;                       /* odd: write in flight */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* A zero duration means "leave that element as it is", so showing the
     * battery on its own does not cut a volume panel short, and vice versa. */
    if (ev->duration_ms) {
        s->kind = ev->kind;
        s->value = ev->value;
        s->flags = ev->flags;
        s->expire_ms = now + ev->duration_ms;
        s->panel_anchor = ev->panel_anchor;
    }
    s->style_flags = ev->style_flags;
    s->fg = ev->fg;
    s->bg = ev->bg;
    s->alpha = ev->alpha;
    s->scale = ev->scale;
    if (ev->battery_duration_ms) {
        s->battery = ev->battery;
        s->battery_flags = ev->battery_flags;
        s->battery_expire_ms = now + ev->battery_duration_ms;
        s->battery_anchor = ev->battery_anchor;
    }
    if (ev->text_duration_ms) {
        size_t n = ev->text ? strlen(ev->text) : 0;

        if (n >= OV_TEXT_MAX)
            n = OV_TEXT_MAX - 1;
        memcpy((void *)s->text, ev->text ? ev->text : "", n);
        s->text[n] = '\0';
        s->text_expire_ms = now + ev->text_duration_ms;
        s->notification_anchor = ev->notification_anchor;
    }
    s->magic = OV_STATE_MAGIC;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->seq++;                       /* even: settled */

    munmap((void *)s, sizeof(ov_state));
    return 0;
}
