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
static void drs_learn(uint32_t w, uint32_t h) { (void)w; (void)h; }

// ---- steady resolution: the game picks its scene size from GPU timestamp queries. With LAS_DRS=1 the
// timestamps it reads back are compressed (durations shrink by LAS_DRS_K), so the GPU always looks fast and
// the game keeps its largest size, with every pass still consistent with the size it chose.
#define MAXPOOLS 64
static unsigned long long s_pools[MAXPOOLS];
static int s_npools;
static unsigned long long s_base;
static long s_squeezed;
static long s_sq_last;
static unsigned s_k = 0;

void vkshim_diag_qpool(long info, long pPool, long res) {
    if (res != 0 || !info || !pPool) return;
    const uint32_t *v = (const uint32_t *)info;      // queryType at byte 20
    if (v[5] != 2) return;                           // VK_QUERY_TYPE_TIMESTAMP
    unsigned long long h = *(const unsigned long long *)pPool;
    for (int i = 0; i < s_npools; i++) if (s_pools[i] == h) return;
    if (s_npools < MAXPOOLS) s_pools[s_npools++] = h;
    __android_log_print(ANDROID_LOG_INFO, TAG, "diag: timestamp query pool created (%u queries)", v[6]);
}

void vkshim_diag_qresults(long pool, long first, long count, long size, long pData, long stride, long flags, long res) {
    if (!drs_on() || !pData || count < 1 || stride < 4) return;
    int mine = 0;
    for (int i = 0; i < s_npools; i++) if (s_pools[i] == (unsigned long long)pool) { mine = 1; break; }
    if (!mine) return;
    if (!s_k) { const char *e = getenv("LAS_DRS_K"); s_k = e ? (unsigned)atoi(e) : 8; if (s_k < 2 || s_k > 64) s_k = 8; }
    int wide = (flags & 1) != 0;
    for (long q = 0; q < count; q++) {
        unsigned char *p = (unsigned char *)pData + q * stride;
        if (wide) {
            unsigned long long x = *(unsigned long long *)p;
            if (!x) continue;
            if (!s_base) s_base = x;
            if (x > s_base) *(unsigned long long *)p = s_base + (x - s_base) / s_k;
        } else {
            unsigned x = *(unsigned *)p;
            if (!x) continue;
            unsigned b = (unsigned)s_base;
            if (!s_base) { s_base = x; b = x; }
            if (x > b) *(unsigned *)p = b + (x - b) / s_k;
        }
        s_squeezed++;
    }
    long t = now_ms();
    if (t - s_sq_last > 5000) {
        s_sq_last = t;
        __android_log_print(ANDROID_LOG_INFO, TAG, "diag: steady resolution: %ld timestamps squeezed by %u so far", s_squeezed, s_k);
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
}
void vkshim_diag_viewport_cnt(long count, long p) { vkshim_diag_viewport(0, count, p); }
void vkshim_diag_scissor(long first, long count, long p) {
    if (!p || count < 1) return;
    uint32_t *v = (uint32_t *)p;     // offset x,y, extent w,h
    note(&s_sc, "scissor", v[2], v[3], 0);
}
void vkshim_diag_renderpass(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;  // renderArea extent at byte 40
    note(&s_ra, "renderarea", v[10], v[11], 0);
    drs_learn(v[10], v[11]);
}
void vkshim_diag_rendering(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;  // renderArea extent at byte 28
    note(&s_ra, "renderarea(dyn)", v[7], v[8], 1);
}
void vkshim_diag_image(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;
    if (v[5] != 1) return;                       // 2D only
    note(&s_im, "image", v[7], v[8], 0);
}
void vkshim_diag_framebuffer(long info) {
    if (!info) return;
    uint32_t *v = (uint32_t *)info;
    note(&s_fb, "framebuffer", v[12], v[13], 0);
    drs_learn(v[12], v[13]);
}
void vkshim_diag_ts_write(void) { s_ts_write++; }
void vkshim_diag_ts_read(void) { s_ts_read++; tick(); }
