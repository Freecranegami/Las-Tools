// Diagnostics for dynamic resolution scaling: logs every distinct scene-pass size the game uses
// (viewport, scissor, render area, framebuffer, image extents) and how often it reads GPU timers.
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TAG "vkshim"
#define SLOTS 4096
typedef struct { uint32_t key[SLOTS]; int n; } seen_t;
static seen_t s_vp, s_sc, s_ra, s_fb, s_im;
static long s_ts_write, s_ts_read, s_ts_last_w, s_ts_last_r;
static long s_t0, s_tlast;
static int s_vp_n;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int isnew(seen_t *s, uint32_t w, uint32_t h, uint32_t extra) {
    if (w > 16384 || h > 16384 || w < 16 || h < 16) return 0;
    uint32_t k = (w * 2654435761u) ^ (h * 40503u) ^ (extra * 97u) ^ 0x9e3779b9u;
    uint32_t i = k & (SLOTS - 1);
    uint32_t tag = (w << 16 | h) ^ (extra << 28) | 1u;
    for (int p = 0; p < SLOTS; p++) {
        uint32_t j = (i + p) & (SLOTS - 1);
        if (s->key[j] == tag) return 0;
        if (!s->key[j]) {
            if (s->n >= 300) return 0;
            s->key[j] = tag;
            s->n++;
            return 1;
        }
    }
    return 0;
}

static void tick(void) {
    long t = now_ms();
    if (!s_t0) { s_t0 = s_tlast = t; }
    if (t - s_tlast >= 2000) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "diag: t=%lds timestamps written +%ld read +%ld (totals %ld/%ld)",
            (t - s_t0) / 1000, s_ts_write - s_ts_last_w, s_ts_read - s_ts_last_r, s_ts_write, s_ts_read);
        s_ts_last_w = s_ts_write; s_ts_last_r = s_ts_read; s_tlast = t;
    }
}

static void note(seen_t *s, const char *what, uint32_t w, uint32_t h, uint32_t extra) {
    if (!isnew(s, w, h, extra)) return;
    tick();
    __android_log_print(ANDROID_LOG_INFO, TAG, "diag: %s %ux%u%s t=%ldms #%d", what, w, h,
        extra ? " (extra)" : "", now_ms() - s_t0, s->n);
}

void vkshim_diag_viewport(long first, long count, long p) {
    if (!p || count < 1) return;
    const float *v = (const float *)p;
    s_vp_n++;
    note(&s_vp, "viewport", (uint32_t)(v[2] + 0.5f), (uint32_t)(v[3] + 0.5f < 0 ? -v[3] : v[3] + 0.5f), 0);
    if ((s_vp_n & 1023) == 0) tick();
}
void vkshim_diag_viewport_cnt(long count, long p) { vkshim_diag_viewport(0, count, p); }
void vkshim_diag_scissor(long first, long count, long p) {
    if (!p || count < 1) return;
    const uint32_t *v = (const uint32_t *)p;     // offset x,y, extent w,h
    note(&s_sc, "scissor", v[2], v[3], 0);
}
void vkshim_diag_renderpass(long info) {
    if (!info) return;
    const uint32_t *v = (const uint32_t *)info;  // renderArea extent at byte 40
    note(&s_ra, "renderarea", v[10], v[11], 0);
}
void vkshim_diag_rendering(long info) {
    if (!info) return;
    const uint32_t *v = (const uint32_t *)info;  // renderArea extent at byte 28
    note(&s_ra, "renderarea(dyn)", v[7], v[8], 1);
}
void vkshim_diag_image(long info) {
    if (!info) return;
    const uint32_t *v = (const uint32_t *)info;
    if (v[5] != 1) return;                       // 2D only
    note(&s_im, "image", v[7], v[8], v[6] ? 0 : 0);
}
void vkshim_diag_framebuffer(long info) {
    if (!info) return;
    const uint32_t *v = (const uint32_t *)info;
    note(&s_fb, "framebuffer", v[12], v[13], 0);
}
void vkshim_diag_ts_write(void) { s_ts_write++; }
void vkshim_diag_ts_read(void) { s_ts_read++; tick(); }
