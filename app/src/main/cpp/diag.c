// Diagnostics for dynamic resolution scaling: logs every distinct scene-pass size the game uses
// (viewport, scissor, render area, framebuffer, image extents) and how often it reads GPU timers.
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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


// ---- fixed render size: the game steps its scene size up and down (dynamic resolution) and makes new
// images/framebuffers for every step. With LAS_DRS=1 every size of that family is rewritten to the largest.
static int s_drs = -1;
static float s_ratio;            // height / width of the scene target, from the first big framebuffer
static uint32_t s_tw, s_th;      // largest size of the family seen
static long s_drs_n, s_drs_last;
static int drs_on(void) {
    if (s_drs < 0) { const char *e = getenv("LAS_DRS"); s_drs = (e && e[0] == '1') ? 1 : 0; }
    return s_drs;
}
static void drs_learn(uint32_t w, uint32_t h) {
    if (!s_ratio && w >= 800 && w > h) { s_ratio = (float)h / (float)w; s_tw = w; s_th = h; }
}
static int in_family(uint32_t w, uint32_t h) {
    if (!s_ratio || w < 600 || w > 16384) return 0;
    float e = (float)h - (float)w * s_ratio;
    return e <= 1.5f && e >= -1.5f;
}
static void drs_fix(uint32_t *w, uint32_t *h) {
    if (!drs_on() || !in_family(*w, *h)) return;
    if (*w > s_tw) { s_tw = *w; s_th = *h; return; }
    if (*w == s_tw) return;
    *w = s_tw; *h = s_th;
    s_drs_n++;
    long t = now_ms();
    if (t - s_drs_last > 5000) {
        s_drs_last = t;
        __android_log_print(ANDROID_LOG_INFO, TAG, "diag: fixed size: %ld sizes rewritten to %ux%u so far", s_drs_n, s_tw, s_th);
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
    float *v = (float *)p;
    s_vp_n++;
    note(&s_vp, "viewport", (uint32_t)(v[2] + 0.5f), (uint32_t)(v[3] + 0.5f < 0 ? -v[3] : v[3] + 0.5f), 0);
    if ((s_vp_n & 1023) == 0) tick();
    if (drs_on()) {
        for (long i = 0; i < count && i < 16; i++) {
            float *q = v + i * 6;
            uint32_t w = (uint32_t)(q[2] + 0.5f), h = (uint32_t)((q[3] < 0 ? -q[3] : q[3]) + 0.5f);
            uint32_t w0 = w, h0 = h;
            drs_fix(&w, &h);
            if (w != w0 || h != h0) { q[2] = (float)w; q[3] = q[3] < 0 ? -(float)h : (float)h; }
        }
    }
}
void vkshim_diag_viewport_cnt(long count, long p) { vkshim_diag_viewport(0, count, p); }
void vkshim_diag_scissor(long first, long count, long p) {
    if (!p || count < 1) return;
    uint32_t *v = (uint32_t *)p;     // offset x,y, extent w,h
    note(&s_sc, "scissor", v[2], v[3], 0);
    if (drs_on()) for (long i = 0; i < count && i < 16; i++) drs_fix(&v[i * 4 + 2], &v[i * 4 + 3]);
}
void vkshim_diag_renderpass(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;  // renderArea extent at byte 40
    note(&s_ra, "renderarea", v[10], v[11], 0);
    drs_learn(v[10], v[11]);
    if (drs_on()) drs_fix(&v[10], &v[11]);
}
void vkshim_diag_rendering(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;  // renderArea extent at byte 28
    note(&s_ra, "renderarea(dyn)", v[7], v[8], 1);
    if (drs_on()) drs_fix(&v[7], &v[8]);
}
void vkshim_diag_image(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;
    if (v[5] != 1) return;                       // 2D only
    note(&s_im, "image", v[7], v[8], 0);
    if (drs_on()) drs_fix(&v[7], &v[8]);
}
void vkshim_diag_framebuffer(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;
    note(&s_fb, "framebuffer", v[12], v[13], 0);
    drs_learn(v[12], v[13]);
    if (drs_on()) drs_fix(&v[12], &v[13]);
}
void vkshim_diag_ts_write(void) { s_ts_write++; }
void vkshim_diag_ts_read(void) { s_ts_read++; tick(); }
