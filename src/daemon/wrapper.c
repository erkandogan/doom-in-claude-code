// doom-in-claude — doomgeneric backend wrapper.
//
// Implements DG_* callbacks so the real Doom engine renders to:
//   $DIC_STATE_DIR/input.fifo   ← keys from Claude Code
//   $DIC_STATE_DIR/frame.ansi   → 24-bit ANSI half-block frame
//   $DIC_STATE_DIR/frame.txt    → ASCII fallback
//   $DIC_STATE_DIR/state.json   → tick / resolution
//   $DIC_STATE_DIR/daemon.pid   → pid
//
// Tick model: real-time. Doom runs at its native ~35 Hz. User input arrives
// asynchronously via the FIFO and is released after ~90 ms (≈ 3 Doom tics) so
// a single typed character like 'w' produces a short walk. The status line
// polls the rendered frame at ~1 Hz. To keep disk IO modest, frames are only
// flushed to disk every ~150 ms.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/doomkeys.h"
#include "doomgeneric/d_player.h"
#include "doomgeneric/doomstat.h"

// Globals from the Doom engine we introspect for state.json.
extern int      gametic;
extern int      gameepisode;
extern int      gamemap;
extern player_t players[MAXPLAYERS];
extern int      consoleplayer;

static void read_size_env(void);
static void init_sextants(void);

// ─── Paths ────────────────────────────────────────────────────────────────

static char STATE_DIR[512];
static char FIFO_PATH[600];
static char FRAME_ANSI[600];
static char FRAME_TXT[600];
static char STATE_JSON[600];
static char PID_FILE[600];
static char LOG_FILE[600];

static void build_paths(void) {
    const char *sd = getenv("DIC_STATE_DIR");
    if (!sd || !*sd) sd = "/tmp/doom-in-claude";
    snprintf(STATE_DIR,  sizeof STATE_DIR,  "%s", sd);
    snprintf(FIFO_PATH,  sizeof FIFO_PATH,  "%s/input.fifo", sd);
    snprintf(FRAME_ANSI, sizeof FRAME_ANSI, "%s/frame.ansi", sd);
    snprintf(FRAME_TXT,  sizeof FRAME_TXT,  "%s/frame.txt",  sd);
    snprintf(STATE_JSON, sizeof STATE_JSON, "%s/state.json", sd);
    snprintf(PID_FILE,   sizeof PID_FILE,   "%s/daemon.pid", sd);
    snprintf(LOG_FILE,   sizeof LOG_FILE,   "%s/daemon.log", sd);
    mkdir(STATE_DIR, 0755);
    mkfifo(FIFO_PATH, 0644);
}

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ─── Input queue + deferred releases ─────────────────────────────────────

typedef struct { uint8_t pressed; uint8_t key; } key_evt_t;
#define QSIZE 512
static key_evt_t q[QSIZE];
static size_t qhead = 0, qtail = 0;

#define RSIZE 128
typedef struct { uint8_t key; uint64_t deadline_ms; } pending_release_t;
static pending_release_t pend[RSIZE];
static size_t pend_count = 0;

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;

static void enq(uint8_t pressed, uint8_t key) {
    size_t next = (qtail + 1) % QSIZE;
    if (next != qhead) { q[qtail] = (key_evt_t){pressed, key}; qtail = next; }
}

static void add_release(uint8_t key, uint64_t when) {
    if (pend_count < RSIZE) pend[pend_count++] = (pending_release_t){key, when};
}

// How long each chat character holds the key for (ms).
static int hold_ms_for_char(char c) {
    if (c == 'f') return 120;   // fire: slightly longer so the trigger registers
    if (c == 'u' || c == ' ') return 160; // use: needs to "tap"
    return 90;                  // movement: ~3 Doom tics
}

static int char_to_key(char c) {
    switch (c) {
        case 'w': return KEY_UPARROW;
        case 's': return KEY_DOWNARROW;
        case 'a': return KEY_STRAFE_L;
        case 'd': return KEY_STRAFE_R;
        case 'q': return KEY_LEFTARROW;
        case 'e': return KEY_RIGHTARROW;
        case 'f': return KEY_FIRE;
        case ' ':
        case 'u': return KEY_USE;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': return c;
        case '\n': case '\r': return KEY_ENTER;
        default:  return 0;
    }
}

static void inject_char(char c) {
    int k = char_to_key(c);
    if (!k) return;
    enq(1, (uint8_t)k);
    add_release((uint8_t)k, now_ms() + hold_ms_for_char(c));
}

static void handle_line(char *line) {
    size_t n = strlen(line);
    while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
    if (!n) return;

    // `go N` / `wait` / `.` are no-ops in real-time mode — the game advances
    // on its own. We keep them accepted for API compatibility with the stub.
    if (!strncmp(line, "go ", 3) || !strcmp(line, "wait") || !strcmp(line, ".")) return;

    // Serialize keys across wall time so "wwwww" actually produces five
    // distinct walk bursts instead of one. Each character presses its key,
    // sleeps for the hold duration + a small release gap, and only then
    // moves on to the next character. Doom tics at 35 Hz, so a 90 ms hold
    // = ~3 tics = one step's worth of forward motion.
    for (size_t i = 0; i < n; i++) {
        int hold = hold_ms_for_char(line[i]);
        pthread_mutex_lock(&qmu);
        inject_char(line[i]);
        pthread_mutex_unlock(&qmu);
        // Wait for the release deadline to pass + a small gap, so the
        // next press is cleanly separated from the previous release.
        struct timespec ts;
        ts.tv_sec  = (hold + 40) / 1000;
        ts.tv_nsec = ((hold + 40) % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
}

static void *fifo_reader(void *arg) {
    (void)arg;
    for (;;) {
        FILE *f = fopen(FIFO_PATH, "r");
        if (!f) { logf_("fifo open failed: %s", strerror(errno)); sleep(1); continue; }
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) handle_line(buf);
        fclose(f);
    }
    return NULL;
}

// ─── DG_ callbacks ───────────────────────────────────────────────────────

void DG_Init(void) {
    build_paths();
    read_size_env();
    init_sextants();
    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    pthread_t th;
    pthread_create(&th, NULL, fifo_reader, NULL);
    pthread_detach(th);
    logf_("doomd started, pid=%d, state=%s", (int)getpid(), STATE_DIR);
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)now_ms();
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    pthread_mutex_lock(&qmu);

    // First surface any deferred releases whose deadline has arrived.
    uint64_t t = now_ms();
    for (size_t i = 0; i < pend_count; ) {
        if (pend[i].deadline_ms <= t) {
            enq(0, pend[i].key);
            pend[i] = pend[--pend_count];
        } else i++;
    }

    if (qhead == qtail) { pthread_mutex_unlock(&qmu); return 0; }
    key_evt_t e = q[qhead];
    qhead = (qhead + 1) % QSIZE;
    pthread_mutex_unlock(&qmu);
    *pressed = e.pressed;
    *doomKey = e.key;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

// ─── ANSI block-char renderers ───────────────────────────────────────────
//
// Two modes share the same per-cell algorithm — sample N sub-pixels, split
// them into two brightness groups, emit a character whose filled pattern
// matches the "on" group with fg = mean(on) and bg = mean(off):
//
//   "quadrant"  2×2 sub-pixels, 16 chars (U+2580..U+259F).           Fast.
//   "sextant"   2×3 sub-pixels, 64 chars (U+1FB00.. + half-blocks).  50 %
//               more vertical density, zero extra character cost.
//
// Sextant is the default; set DIC_RENDER=quadrant to fall back on terminals
// that can't render U+1FB00..U+1FB3B.
//
// Size is runtime-configurable via DIC_COLS / DIC_ROWS.

#define DEFAULT_COLS 120
#define DEFAULT_ROWS 28
#define FRAME_INTERVAL_MS 300
#define PNG_INTERVAL_MS   1000   /* emit frame.png at most 1 Hz */

static int COLS = DEFAULT_COLS;
static int ROWS = DEFAULT_ROWS;
enum render_mode {
    RENDER_QUADRANT,
    RENDER_SEXTANT,
    RENDER_SIXEL,
    RENDER_CHAFA,
    RENDER_TIMG,         /* timg binary (brew install timg) — alt block renderer */
    RENDER_BLOCKS_HQ,    /* native HQ block renderer: area-averaged colors, safe glyphs */
};
static int RENDER = RENDER_SEXTANT;
static int CHAFA_AVAILABLE = 0;
static int TIMG_AVAILABLE = 0;
static const char *PNG_TOOL = NULL;  /* "sips" | "magick" | "convert" | NULL */
static uint64_t last_png_ms = 0;
static int SIXEL_W = 320;
static int SIXEL_H = 200;
static uint64_t last_frame_ms = 0;

/* Gamma applied to pixels before 4-bit quantization + chafa color-mapping.
 * Values < 1.0 BRIGHTEN midtones (recommended for Doom's dark scenes —
 * shadow detail that would otherwise collapse to palette index 0 gets
 * lifted into a distinguishable cube entry). Values > 1.0 darken. Default
 * 1.0 = no change. Override via DIC_GAMMA env or --gamma CLI flag.
 * Useful range: 0.5 (dramatic brighten) → 1.0 (off) → 1.5 (darker).
 * Good starting point for Doom: 0.6-0.7 */
static double GAMMA_PRE = 1.0;

/* Precomputed 256-entry LUT for `GAMMA_PRE` — rebuilt whenever GAMMA_PRE
 * changes. Applying gamma per-pixel via pow() at 640×336 pixels × 3.3 Hz
 * would be 5M pow() calls/sec; LUT reduces that to 5M table lookups. */
static uint8_t GAMMA_LUT[256];
static double GAMMA_LUT_FOR = 1.0;  /* gamma value this LUT was built for */

static void rebuild_gamma_lut(void) {
    /* Standard image-editing convention: output = (input/255)^gamma * 255.
     * gamma < 1.0 brightens midtones (good for dark Doom scenes).
     * gamma > 1.0 darkens. Example with gamma=0.7:
     *   input 64  → output 95   (a dark corridor wall lifts out of pure black)
     *   input 128 → output 162  (midtones brighten moderately)
     *   input 192 → output 215  (brights barely changed) */
    for (int i = 0; i < 256; i++) {
        double v = pow(i / 255.0, GAMMA_PRE) * 255.0;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        GAMMA_LUT[i] = (uint8_t)(v + 0.5);
    }
    GAMMA_LUT_FOR = GAMMA_PRE;
}

// UTF-8 for the 16 quadrant chars (bit order TL=8 TR=4 BL=2 BR=1).
static const char *QUAD_CHARS[16] = {
    " ",
    "\xe2\x96\x97", "\xe2\x96\x96", "\xe2\x96\x84",
    "\xe2\x96\x9d", "\xe2\x96\x90", "\xe2\x96\x9e", "\xe2\x96\x9f",
    "\xe2\x96\x98", "\xe2\x96\x9a", "\xe2\x96\x8c", "\xe2\x96\x99",
    "\xe2\x96\x80", "\xe2\x96\x9c", "\xe2\x96\x9b", "\xe2\x96\x88",
};

// UTF-8 table for the 64 sextant patterns, indexed 0..63.
// Bit layout: bit0=TL bit1=TR bit2=ML bit3=MR bit4=BL bit5=BR.
// The Unicode range U+1FB00..U+1FB3B covers 60 of 64 cases; patterns 0, 21,
// 42, 63 are served by SPACE, ▌, ▐, █ respectively. Built once at init.
static char SEXTANT[64][5];

static void init_sextants(void) {
    for (int b = 0; b < 64; b++) {
        uint32_t cp;
        if      (b == 0)  cp = 0x0020;  // space
        else if (b == 21) cp = 0x258C;  // ▌
        else if (b == 42) cp = 0x2590;  // ▐
        else if (b == 63) cp = 0x2588;  // █
        else {
            int seq;
            if      (b < 21) seq = b - 1;
            else if (b < 42) seq = b - 2;
            else             seq = b - 3;
            cp = 0x1FB00 + seq;
        }
        char *p = SEXTANT[b];
        if (cp < 0x80) {
            *p++ = (char)cp;
        } else if (cp < 0x800) {
            *p++ = (char)(0xC0 | (cp >> 6));
            *p++ = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *p++ = (char)(0xE0 | (cp >> 12));
            *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *p++ = (char)(0x80 | (cp & 0x3F));
        } else {
            *p++ = (char)(0xF0 | (cp >> 18));
            *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *p++ = (char)(0x80 | (cp & 0x3F));
        }
        *p = 0;
    }
}

static void read_size_env(void) {
    const char *c = getenv("DIC_COLS"); if (c && atoi(c) > 0) COLS = atoi(c);
    const char *r = getenv("DIC_ROWS"); if (r && atoi(r) > 0) ROWS = atoi(r);
    if (COLS < 20)  COLS = 20;
    if (COLS > 400) COLS = 400;
    if (ROWS < 8)   ROWS = 8;
    if (ROWS > 120) ROWS = 120;
    // Probe for chafa and timg binaries. chafa remains the recommended
    // default because it emits byte-stable output through our perl
    // normalizer AND sticks to safe Unicode 1.1 block glyphs
    // (U+2580..U+259F). Sextant mode is opt-in only: Ink's string-width
    // can't measure the Unicode 13+ sextant glyphs reliably.
    CHAFA_AVAILABLE = (system("command -v chafa >/dev/null 2>&1") == 0);
    TIMG_AVAILABLE  = (system("command -v timg  >/dev/null 2>&1") == 0);
    const char *m = getenv("DIC_RENDER");
    if      (m && !strcmp(m, "quadrant"))  RENDER = RENDER_QUADRANT;
    else if (m && !strcmp(m, "sextant"))   RENDER = RENDER_SEXTANT;
    else if (m && !strcmp(m, "sixel"))     RENDER = RENDER_SIXEL;
    else if (m && !strcmp(m, "chafa"))     RENDER = RENDER_CHAFA;
    else if (m && !strcmp(m, "timg"))      RENDER = RENDER_TIMG;
    else if (m && (!strcmp(m, "blocks") || !strcmp(m, "blocks-hq") ||
                   !strcmp(m, "doom")   || !strcmp(m, "native")))
                                           RENDER = RENDER_BLOCKS_HQ;
    else if (CHAFA_AVAILABLE)              RENDER = RENDER_CHAFA;
    else                                   RENDER = RENDER_QUADRANT;

    if (RENDER == RENDER_CHAFA && !CHAFA_AVAILABLE) {
        fprintf(stderr, "doom-in-claude: chafa not installed — falling back to quadrant. Install with: brew install chafa\n");
        RENDER = RENDER_QUADRANT;
    }
    if (RENDER == RENDER_TIMG && !TIMG_AVAILABLE) {
        fprintf(stderr, "doom-in-claude: timg not installed — falling back to blocks-hq. Install with: brew install timg\n");
        RENDER = RENDER_BLOCKS_HQ;
    }

    // Probe for a PNG converter for frame.png (used by the MCP doom_look
    // tool so Claude can SEE the scene during autoplay, not just read
    // ASCII). sips is built into macOS; magick/convert on Linux. If none
    // of the above is present we skip PNG generation silently — MCP
    // gracefully falls back to text-only.
    if      (system("command -v sips    >/dev/null 2>&1") == 0) PNG_TOOL = "sips";
    else if (system("command -v magick  >/dev/null 2>&1") == 0) PNG_TOOL = "magick";
    else if (system("command -v convert >/dev/null 2>&1") == 0) PNG_TOOL = "convert";

    const char *sw = getenv("DIC_SIXEL_W"); if (sw && atoi(sw) > 0) SIXEL_W = atoi(sw);
    const char *sh = getenv("DIC_SIXEL_H"); if (sh && atoi(sh) > 0) SIXEL_H = atoi(sh);
    if (SIXEL_W < 64 || SIXEL_W > 640) SIXEL_W = 320;
    if (SIXEL_H < 48 || SIXEL_H > 400) SIXEL_H = 200;

    const char *gg = getenv("DIC_GAMMA");
    if (gg && *gg) {
        double g = atof(gg);
        if (g >= 0.1 && g <= 4.0) GAMMA_PRE = g;
    }
    rebuild_gamma_lut();
}

static inline void argb_to_rgb(uint32_t px, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (px >> 16) & 0xff;
    *g = (px >>  8) & 0xff;
    *b = (px      ) & 0xff;
}

static inline int brightness(uint8_t r, uint8_t g, uint8_t b) {
    return (r * 54 + g * 183 + b * 19) >> 8;
}

// Pick fg/bg from N sampled sub-pixels. Returns the bit pattern (LSB = first
// sample). Caller interprets the pattern against its subpixel layout.
static int classify_cell(
    int n,
    const uint8_t *r, const uint8_t *g, const uint8_t *b, const int *lum,
    int *fgR, int *fgG, int *fgB,
    int *bgR, int *bgG, int *bgB
) {
    int avg = 0;
    for (int i = 0; i < n; i++) avg += lum[i];
    avg /= n;

    int pattern = 0, fg = 0, bg = 0;
    int frR = 0, frG = 0, frB = 0, brR = 0, brG = 0, brB = 0;
    for (int i = 0; i < n; i++) {
        if (lum[i] >= avg) {
            pattern |= (1 << i);
            frR += r[i]; frG += g[i]; frB += b[i];
            fg++;
        } else {
            brR += r[i]; brG += g[i]; brB += b[i];
            bg++;
        }
    }
    if (fg == 0) { frR = brR / bg; frG = brG / bg; frB = brB / bg; fg = 1; }
    if (bg == 0) { brR = frR / fg; brG = frG / fg; brB = frB / fg; bg = 1; }
    *fgR = frR / fg; *fgG = frG / fg; *fgB = frB / fg;
    *bgR = brR / bg; *bgG = brG / bg; *bgB = brB / bg;
    return pattern;
}

static void render_sextant(FILE *fa, FILE *ft) {
    uint32_t *sb = DG_ScreenBuffer;
    int sw = DOOMGENERIC_RESX;
    int sh = DOOMGENERIC_RESY;
    double sx_scale = (double)sw / (COLS * 2);
    double sy_scale = (double)sh / (ROWS * 3);

    fprintf(fa, "\x1b[1m\x1b[38;2;255;60;60mDOOM\x1b[0m  tick %d  %dx%d  sextant\n",
            gametic, COLS, ROWS);
    fprintf(ft, "DOOM  tick %d  %dx%d\n", gametic, COLS, ROWS);

    // Sub-pixel order matches sextant bit layout: 0=TL 1=TR 2=ML 3=MR 4=BL 5=BR.
    static const int SX[6] = { 0, 1, 0, 1, 0, 1 };
    static const int SY[6] = { 0, 0, 1, 1, 2, 2 };

    for (int ry = 0; ry < ROWS; ry++) {
        for (int cx = 0; cx < COLS; cx++) {
            uint8_t r[6], g[6], b[6]; int lum[6];
            for (int i = 0; i < 6; i++) {
                int sx = (int)((cx * 2 + SX[i] + 0.5) * sx_scale);
                int sy = (int)((ry * 3 + SY[i] + 0.5) * sy_scale);
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                argb_to_rgb(sb[sy * sw + sx], &r[i], &g[i], &b[i]);
                lum[i] = brightness(r[i], g[i], b[i]);
            }
            int fR, fG, fB, bR, bG, bB;
            int p = classify_cell(6, r, g, b, lum, &fR, &fG, &fB, &bR, &bG, &bB);
            fprintf(fa,
                    "\x1b[38;2;%03d;%03d;%03dm\x1b[48;2;%03d;%03d;%03dm%s",
                    fR, fG, fB, bR, bG, bB, SEXTANT[p]);
            int avg = (lum[0]+lum[1]+lum[2]+lum[3]+lum[4]+lum[5]) / 6;
            static const char ramp[] = " .:-=+*#%@";
            fputc(ramp[(avg * (int)(sizeof ramp - 2)) / 256], ft);
        }
        fprintf(fa, "\x1b[0m\n");
        fputc('\n', ft);
    }
}

static void render_quadrant(FILE *fa, FILE *ft) {
    uint32_t *sb = DG_ScreenBuffer;
    int sw = DOOMGENERIC_RESX;
    int sh = DOOMGENERIC_RESY;
    double sx_scale = (double)sw / (COLS * 2);
    double sy_scale = (double)sh / (ROWS * 2);

    fprintf(fa, "\x1b[1m\x1b[38;2;255;60;60mDOOM\x1b[0m  tick %d  %dx%d  quadrant\n",
            gametic, COLS, ROWS);
    fprintf(ft, "DOOM  tick %d  %dx%d\n", gametic, COLS, ROWS);

    for (int ry = 0; ry < ROWS; ry++) {
        for (int cx = 0; cx < COLS; cx++) {
            uint8_t r[4], g[4], bb[4]; int lum[4];
            for (int i = 0; i < 4; i++) {
                int dx = i & 1, dy = (i >> 1) & 1;
                int sx = (int)((cx * 2 + dx + 0.5) * sx_scale);
                int sy = (int)((ry * 2 + dy + 0.5) * sy_scale);
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                argb_to_rgb(sb[sy * sw + sx], &r[i], &g[i], &bb[i]);
                lum[i] = brightness(r[i], g[i], bb[i]);
            }
            int fR, fG, fB, bR, bG, bB;
            int p = classify_cell(4, r, g, bb, lum, &fR, &fG, &fB, &bR, &bG, &bB);
            // Remap classify_cell's LSB-first pattern to QUAD_CHARS bit order
            // (TL=8 TR=4 BL=2 BR=1). classify bit i corresponds to sub i:
            //   sub 0 = TL (bit 8), sub 1 = TR (bit 4),
            //   sub 2 = BL (bit 2), sub 3 = BR (bit 1).
            int q = 0;
            if (p & 1) q |= 8;
            if (p & 2) q |= 4;
            if (p & 4) q |= 2;
            if (p & 8) q |= 1;
            fprintf(fa,
                    "\x1b[38;2;%03d;%03d;%03dm\x1b[48;2;%03d;%03d;%03dm%s",
                    fR, fG, fB, bR, bG, bB, QUAD_CHARS[q]);
            int avg = (lum[0]+lum[1]+lum[2]+lum[3]) / 4;
            static const char ramp[] = " .:-=+*#%@";
            fputc(ramp[(avg * (int)(sizeof ramp - 2)) / 256], ft);
        }
        fprintf(fa, "\x1b[0m\n");
        fputc('\n', ft);
    }
}

// ─── Sixel renderer ──────────────────────────────────────────────────────
//
// Emits a DECsixel image: real pixels, no block-character grid. Quantizes
// the framebuffer to an R3:G3:B2 256-color palette and writes one color-
// plane scan per used color per 6-row band.
//
// Sixel in Claude Code's status line is experimental. Risks:
//   - Claude Code's Ink renderer measures output width; sixel bytes may be
//     counted as text and break layout.
//   - Claude Code's next UI redraw may overwrite the image.
// If it renders cleanly in your terminal, great. If not, switch back to
// sextant with `--render sextant` or the DIC_RENDER env var.

static void render_sixel(FILE *fa, FILE *ft) {
    uint32_t *sb = DG_ScreenBuffer;
    int sw = DOOMGENERIC_RESX, shh = DOOMGENERIC_RESY;
    int ow = SIXEL_W, oh = SIXEL_H;
    int bands = (oh + 5) / 6;

    uint8_t *q = (uint8_t*)malloc((size_t)ow * oh);
    if (!q) return;
    int used[256]; memset(used, 0, sizeof used);

    for (int y = 0; y < oh; y++) {
        int syy = y * shh / oh;
        for (int x = 0; x < ow; x++) {
            int sxx = x * sw / ow;
            uint32_t px = sb[syy * sw + sxx];
            uint8_t R = (px >> 16) & 0xFF;
            uint8_t G = (px >>  8) & 0xFF;
            uint8_t B = (px      ) & 0xFF;
            uint8_t idx = (uint8_t)((R & 0xE0) | ((G & 0xE0) >> 3) | (B >> 6));
            q[y * ow + x] = idx;
            used[idx] = 1;
        }
    }

    // Open DCS + raster attributes.
    fputs("\x1bPq", fa);
    fprintf(fa, "\"1;1;%d;%d", ow, oh);

    // Emit palette (only colors that actually appear).
    int remap[256];
    int pal_n = 0;
    for (int i = 0; i < 256; i++) {
        if (!used[i]) continue;
        remap[i] = pal_n;
        int R = (i & 0xE0);
        int G = (i & 0x1C) << 3;
        int B = (i & 0x03) << 6;
        fprintf(fa, "#%d;2;%d;%d;%d",
                pal_n, R * 100 / 255, G * 100 / 255, B * 100 / 255);
        pal_n++;
    }

    // Per-color scan per band. Uses a small "present" cache so we skip
    // colors that don't appear in the current band.
    uint8_t present[256];
    for (int band = 0; band < bands; band++) {
        int y0 = band * 6;
        int y1 = y0 + 6; if (y1 > oh) y1 = oh;
        memset(present, 0, sizeof present);
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < ow; x++)
                present[q[y * ow + x]] = 1;

        for (int i = 0; i < 256; i++) {
            if (!used[i] || !present[i]) continue;
            fprintf(fa, "#%d", remap[i]);
            for (int x = 0; x < ow; x++) {
                uint8_t bits = 0;
                for (int y = y0; y < y1; y++) {
                    if (q[y * ow + x] == i) bits |= (1 << (y - y0));
                }
                fputc(0x3F + bits, fa);
            }
            fputc('$', fa);   // CR: stay on this band, next color overlays
        }
        fputc('-', fa);       // NL: advance to next 6-pixel band
    }

    // End DCS.
    fputs("\x1b\\", fa);

    free(q);

    // frame.txt: keep a compact ASCII header so MCP doom_look still has
    // something meaningful for clients that don't support sixel.
    fprintf(ft, "DOOM  tick %d  sixel %dx%d\n", gametic, ow, oh);
}

// ─── Shared renderer helpers (chafa / timg / blocks-hq) ─────────────────
//
// Three block-based renderers share the same pre-processing:
//   1. Crop Doom's native HUD from the bottom of the framebuffer (we
//      render our own text HUD at higher fidelity).
//   2. Blit a black border into the cropped image so edge cells never
//      change frame-to-frame — stable anchors for Ink's diff.
//   3. Build a fixed-width byte-stable HUD line for Ink.
//
// These helpers centralize that so the three renderers stay consistent.

#define VIEWPORT_CROP_ROWS 64   /* Doom's 32-row native HUD × 2× scale */
#define VIEWPORT_BORDER_PX 8    /* static-black anchor border width */

/* Dump the framebuffer as a bordered PPM, with the native Doom HUD cropped
 * off. Writes P6 (binary) PPM to `path`. Returns the ppm height written. */
static int dump_ppm_viewport(const char *path) {
    int ppm_h = DOOMGENERIC_RESY - VIEWPORT_CROP_ROWS;
    FILE *p = fopen(path, "wb");
    if (!p) return 0;
    fprintf(p, "P6\n%d %d\n255\n", DOOMGENERIC_RESX, ppm_h);
    uint32_t *sb = DG_ScreenBuffer;
    for (int y = 0; y < ppm_h; y++) {
        int y_border = y < VIEWPORT_BORDER_PX || y >= ppm_h - VIEWPORT_BORDER_PX;
        for (int x = 0; x < DOOMGENERIC_RESX; x++) {
            int x_border = x < VIEWPORT_BORDER_PX || x >= DOOMGENERIC_RESX - VIEWPORT_BORDER_PX;
            uint8_t bb[3];
            if (x_border || y_border) {
                bb[0] = bb[1] = bb[2] = 0;
            } else {
                /* Apply gamma LUT first (brightens midtones if GAMMA_PRE<1,
                 * a common fix for Doom's dark corridors collapsing into
                 * the single black cube entry), then snap to 4-bit-per-
                 * channel grid to collapse sub-pixel interpolation noise. */
                uint32_t px = sb[y * DOOMGENERIC_RESX + x];
                uint8_t r = GAMMA_LUT[(px >> 16) & 0xFF];
                uint8_t g = GAMMA_LUT[(px >>  8) & 0xFF];
                uint8_t b = GAMMA_LUT[(px      ) & 0xFF];
                bb[0] = r & 0xF0;
                bb[1] = g & 0xF0;
                bb[2] = b & 0xF0;
            }
            fwrite(bb, 1, 3, p);
        }
    }
    fclose(p);
    return ppm_h;
}

/* Fill `buf` with the fixed-width byte-stable rich HUD line. Every field
 * is padded to its max width so the HUD byte count stays identical
 * regardless of HP, weapon, ammo, or kill count changes — otherwise Ink's
 * diff drifts on every stat update. */
static void make_hud_line(char *buf, size_t bufsz) {
    player_t *pl = &players[consoleplayer];
    int hp = pl->health;
    int armor = pl->armorpoints;
    int w_idx = (pl->readyweapon >= 0 && pl->readyweapon < NUMWEAPONS) ? pl->readyweapon : 0;
    static const char *WEAPON_SHORT[NUMWEAPONS] = {
        "FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKET",
        "PLASMA", "BFG", "CHAINSAW", "SSHOTGUN",
    };
    static const int WEAPON_AMMO[NUMWEAPONS] = {
        am_noammo, am_clip, am_shell, am_clip, am_misl,
        am_cell, am_cell, am_noammo, am_shell,
    };
    int at = WEAPON_AMMO[w_idx];
    int cur_ammo = (at == am_noammo) ? -1 : pl->ammo[at];
    int max_ammo = (at == am_noammo) ? -1 : pl->maxammo[at];

    const char *hp_color   = hp > 66 ? "\033[38;2;120;220;120m"
                           : hp > 33 ? "\033[38;2;255;220;80m"
                                     : "\033[38;2;255;80;80m";
    const char *arm_color  = armor > 100 ? "\033[38;2;120;180;255m"
                           : armor > 0   ? "\033[38;2;180;180;255m"
                                         : "\033[38;2;120;120;120m";
    const char *wpn_color  = "\033[38;2;255;200;80m";
    const char *ammo_color = (cur_ammo < 0) ? "\033[38;2;120;120;120m"
                           : (cur_ammo == 0) ? "\033[38;2;255;80;80m"
                                             : "\033[38;2;220;220;220m";
    const char *lvl_color  = "\033[38;2;255;60;60m";
    const char *meta_color = "\033[38;2;120;160;200m";

    char ammo_field[16];
    if (cur_ammo >= 0) {
        snprintf(ammo_field, sizeof ammo_field, "%3d/%3d", cur_ammo, max_ammo);
    } else {
        snprintf(ammo_field, sizeof ammo_field, "  -/  -");
    }
    snprintf(buf, bufsz,
        "\033[0m%sDOOM\033[0m %sE%dM%-2d\033[0m  "
        "%sHP %3d\033[0m  %sARM %3d\033[0m  "
        "%s%-8s\033[0m %s%s\033[0m  "
        "%sKILLS %4d  ITEMS %4d  SECRETS %3d\033[0m",
        lvl_color, meta_color, gameepisode, gamemap,
        hp_color, hp, arm_color, armor,
        wpn_color, WEAPON_SHORT[w_idx], ammo_color, ammo_field,
        meta_color, pl->killcount, pl->itemcount, pl->secretcount);
}

// ─── Chafa delegation ────────────────────────────────────────────────────
//
// chafa (https://hpjansson.org/chafa/) is the gold standard for image →
// terminal-graphics conversion: auto symbol selection, proper dithering,
// palette optimization, and first-class support for sixel / kitty / iterm2.
// Much higher quality than our hand-rolled renderers.
//
// We dump the framebuffer as a binary PPM and shell out to chafa. Output
// is written atomically to frame.ansi. Options are overridable via
// CHAFA_OPTS env (default: sextant symbols, 24-bit color, foreground-only).

/* Write the current framebuffer as a PPM to the given path. Returns 1 on
 * success, 0 otherwise. */
static int dump_ppm(const char *path) {
    FILE *p = fopen(path, "wb");
    if (!p) return 0;
    fprintf(p, "P6\n%d %d\n255\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    uint32_t *sb = DG_ScreenBuffer;
    int n = DOOMGENERIC_RESX * DOOMGENERIC_RESY;
    for (int i = 0; i < n; i++) {
        uint32_t px = sb[i];
        uint8_t bb[3] = {
            (uint8_t)((px >> 16) & 0xFF),
            (uint8_t)((px >>  8) & 0xFF),
            (uint8_t)((px      ) & 0xFF),
        };
        fwrite(bb, 1, 3, p);
    }
    fclose(p);
    return 1;
}

// Rate-limited PNG emission. Backgrounded so it doesn't block rendering.
// If ppm_path is NULL we dump a fresh PPM first; otherwise we trust the
// caller's existing PPM (render_chafa_direct already writes one).
static void emit_png_if_due(const char *ppm_path) {
    if (!PNG_TOOL) return;
    uint64_t t = now_ms();
    if (t - last_png_ms < PNG_INTERVAL_MS) return;
    last_png_ms = t;

    char our_ppm[700];
    if (!ppm_path) {
        snprintf(our_ppm, sizeof our_ppm, "%s/frame.ppm", STATE_DIR);
        if (!dump_ppm(our_ppm)) return;
        ppm_path = our_ppm;
    }

    char png_out[700];
    snprintf(png_out, sizeof png_out, "%s/frame.png", STATE_DIR);
    char cmd[900];
    if (!strcmp(PNG_TOOL, "sips")) {
        snprintf(cmd, sizeof cmd,
            "sips -s format png '%s' --out '%s.tmp' >/dev/null 2>&1 && mv '%s.tmp' '%s' &",
            ppm_path, png_out, png_out, png_out);
    } else {
        /* magick / convert share the same syntax for PPM→PNG */
        snprintf(cmd, sizeof cmd,
            "%s '%s' '%s.tmp' >/dev/null 2>&1 && mv '%s.tmp' '%s' &",
            PNG_TOOL, ppm_path, png_out, png_out, png_out);
    }
    system(cmd);
}

static void render_chafa_direct(void) {
    char ppm_path[700];
    snprintf(ppm_path, sizeof ppm_path, "%s/frame.ppm", STATE_DIR);
    if (!dump_ppm_viewport(ppm_path)) return;

    const char *opts = getenv("CHAFA_OPTS");
    if (!opts || !*opts) {
        // Balanced default: 256-palette + ordered dither + din99d.
        // ~16B per-cell SGR (half of TrueColor's 32B), ordered pattern
        // doesn't "crawl" frame-to-frame like diffusion, din99d maps
        // Doom's muted colors to perceptually-close palette entries.
        // Combined with `export CLAUDE_CODE_TMUX_TRUECOLOR=1` at shell
        // level (which keeps Claude Code's OWN UI in TrueColor), this
        // is the drift/fidelity sweet spot. To override for full
        // TrueColor DOOM output, set CHAFA_OPTS env or use
        // --chafa-preset vivid.
        opts = "--symbols quad+block+vhalf+hhalf --format symbols "
               "--colors 256 --dither ordered --dither-grain 4x4 --work 9 "
               "--color-extractor median --color-space din99d "
               "--optimize 0";
    }

    char hud[600];
    make_hud_line(hud, sizeof hud);

    // Write the HUD + chafa output atomically.
    //
    // Pipe chafa through a perl normalizer that makes every cell
    // byte-stable across frames. The old sed+awk pipeline stripped DEC
    // private modes (good) but left RGB values as 1-3 digits, so each
    // cell's byte length varied frame-to-frame even when visible width
    // stayed constant. That variable byte structure is what Ink's
    // per-cell diff engine ultimately drifts on — it's the last
    // remaining source of "corruption accumulates until Claude Code
    // restart" after we'd already locked in safe glyphs and pure SGR
    // output.
    //
    // Normalizer steps:
    //   1. Strip DEC private modes \e[?...  (Ink doesn't track them).
    //   2. Strip \e[K (clear-to-EOL — same reason).
    //   3. Pad every ";2;R;G;B" RGB triple AND every ";5;N" palette
    //      index to 3 digits each. With --colors 256 the format becomes
    //      \e[38;5;NNN;48;5;NNNm — same 3-digit padding guarantees
    //      byte-identical cell structure across frames.
    //   4. Collapse runs of \e[0m into one reset.
    //   5. Replace every 1-byte ASCII space with 3-byte U+2800
    //      (BRAILLE PATTERN BLANK — same visual: fully blank, width 1).
    //      Chafa picks SPACE vs block per cell, and mixing 1-byte and
    //      3-byte glyphs is the other source of inter-frame byte drift
    //      once RGB is padded. The HUD line is printed separately by
    //      the parent shell block (before chafa's pipe), so it never
    //      reaches perl — we can blanket-replace every space we see.
    //   6. Guarantee each line ends with exactly one \e[0m\n.
    // Detect if the user is requesting a binary image protocol
    // (sixel / kitty). Those output formats are multi-line DCS / APC
    // blobs that must be passed through untouched — the byte-stability
    // normalizer destroys them by adding `\e[0m\n` at every line end.
    // For image formats we skip the normalizer entirely and only strip
    // the opening cursor-hide (\x1b[?25l) + the matching cursor-show
    // (\x1b[?25h) that chafa emits around the image.
    int is_image_format = (strstr(opts, "sixel") != NULL) ||
                          (strstr(opts, "kitty") != NULL);

    char cmd[4000];
    if (is_image_format) {
        // `tr -d` removes the cursor hide/show single bytes if they
        // leaked through. We also skip the HUD on image frames: mixing
        // CSI text and DCS/APC in Ink's statusline reliably breaks
        // layout because Ink counts every byte of the image data as a
        // visible character. Pure image output has the best shot at
        // being passed through untouched.
        snprintf(cmd, sizeof cmd,
            "chafa --size %dx%d %s '%s' 2>>'%s' "
            "  | perl -0777 -pe 's/\\e\\[\\?25[lh]//g' "
            "  > '%s.tmp' && mv '%s.tmp' '%s'",
            COLS, ROWS, opts, ppm_path, LOG_FILE,
            FRAME_ANSI, FRAME_ANSI, FRAME_ANSI);
    } else {
        snprintf(cmd, sizeof cmd,
            "{ printf '%%s\\n' '%s'; "
            "  chafa --size %dx%d %s '%s' 2>>'%s' | "
            "  perl -CS -pe '%s'; "
            "} > '%s.tmp' && mv '%s.tmp' '%s'",
            hud,
            COLS, ROWS, opts, ppm_path, LOG_FILE,
            "s/\\e\\[\\?[0-9;]*[A-Za-z]//g;"
            "s/\\e\\[K//g;"
            "s{\\e\\[([0-9;]+)m}{"
                "my $p=$1;"
                "$p=~s{(?<![0-9])(38|48);2;(\\d+);(\\d+);(\\d+)}{sprintf(\"%s;2;%03d;%03d;%03d\",$1,$2,$3,$4)}ge;"
                "$p=~s{(?<![0-9])(38|48);5;(\\d+)}{sprintf(\"%s;5;%03d\",$1,$2)}ge;"
                "\"\\e[${p}m\""
            "}ge;"
            "s/(?:\\e\\[0m){2,}/\\e[0m/g;"
            "s/ /\\x{2800}/g;"
            "s/(\\e\\[0m)*(\\n?)\\z/\\e[0m\\n/",
            FRAME_ANSI, FRAME_ANSI, FRAME_ANSI);
    }
    int rc = system(cmd);
    if (rc != 0) {
        static int warned = 0;
        if (!warned) { logf_("chafa invocation failed (rc=%d); falling back", rc); warned = 1; }
        RENDER = RENDER_SEXTANT;
    }

    // Always update the plain-ASCII fallback so the MCP doom_look tool still
    // has something for non-sixel clients.
    FILE *ft = fopen(FRAME_TXT, "w");
    if (ft) { fprintf(ft, "DOOM  tick %d  chafa %dx%d\n", gametic, COLS, ROWS); fclose(ft); }
}

// ─── timg delegation ─────────────────────────────────────────────────────
//
// timg (https://github.com/hzeller/timg) is an alternative terminal image
// renderer with different color quantization and dithering characteristics.
// Often looks cleaner than chafa on dark scenes because timg uses chroma
// subsampling + area-averaged colors rather than Floyd-Steinberg dithering.
//
// Invocation: reuse the same bordered PPM we write for chafa, pipe to timg
// with quarter-block pixelation for 2×2 subpixel resolution per cell. Pipe
// through the same perl normalizer as chafa so the output is byte-stable.

static void render_timg_direct(void) {
    char ppm_path[700];
    snprintf(ppm_path, sizeof ppm_path, "%s/frame.ppm", STATE_DIR);
    if (!dump_ppm_viewport(ppm_path)) return;

    char hud[600];
    make_hud_line(hud, sizeof hud);

    // timg flags:
    //   -g WxH            — output size (cells)
    //   --pixelation=q    — quarter-block (2×2 subpixels per cell, safe glyphs)
    //   --frames=1        — render once and exit
    //   -f                — exit on finish (don't try to animate)
    //   --clear=first     — skip clearing; we're writing to a file
    //                       (timg sometimes emits control chars for tty use)
    // We don't use -U (upper-block only) because it loses the background
    // color information we want for full DOOM colors.
    const char *timg_opts = getenv("TIMG_OPTS");
    if (!timg_opts || !*timg_opts) {
        timg_opts = "--pixelation=quarter --frames=1 -f";
    }

    char cmd[4000];
    snprintf(cmd, sizeof cmd,
        "{ printf '%%s\\n' '%s'; "
        "  timg -g %dx%d %s '%s' 2>>'%s' | "
        "  perl -CS -pe '%s'; "
        "} > '%s.tmp' && mv '%s.tmp' '%s'",
        hud,
        COLS, ROWS, timg_opts, ppm_path, LOG_FILE,
        "s/\\e\\[\\?[0-9;]*[A-Za-z]//g;"
        "s/\\e\\[K//g;"
        "s{\\e\\[([0-9;]+)m}{"
            "my $p=$1;"
            "$p=~s{(?<![0-9])(38|48);2;(\\d+);(\\d+);(\\d+)}{sprintf(\"%s;2;%03d;%03d;%03d\",$1,$2,$3,$4)}ge;"
            "$p=~s{(?<![0-9])(38|48);5;(\\d+)}{sprintf(\"%s;5;%03d\",$1,$2)}ge;"
            "\"\\e[${p}m\""
        "}ge;"
        "s/(?:\\e\\[0m){2,}/\\e[0m/g;"
        "s/ /\\x{2800}/g;"
        "s/(\\e\\[0m)*(\\n?)\\z/\\e[0m\\n/",
        FRAME_ANSI, FRAME_ANSI, FRAME_ANSI);
    int rc = system(cmd);
    if (rc != 0) {
        static int warned = 0;
        if (!warned) { logf_("timg invocation failed (rc=%d); falling back to blocks-hq", rc); warned = 1; }
        RENDER = RENDER_BLOCKS_HQ;
    }

    FILE *ft = fopen(FRAME_TXT, "w");
    if (ft) { fprintf(ft, "DOOM  tick %d  timg %dx%d\n", gametic, COLS, ROWS); fclose(ft); }
}

// ─── Native "blocks HQ" renderer ─────────────────────────────────────────
//
// Purpose-built for Doom inside Ink:
//   - 2×2 subpixel density using only Unicode 1.1 block glyphs (U+2580..).
//   - Per sub-region: AVERAGE all pixels covered by the sub-region instead
//     of point-sampling one. Eliminates the granular sampling noise the
//     simpler render_quadrant shows when each cell covers many pixels.
//   - 2-color split via brightness median (same as classify_cell).
//   - Rich fixed-width HUD (shared with chafa / timg).
//   - Static black border anchor matching the PPM border.
//   - 3-digit padded RGB and trailing \e[0m\n — fully byte-stable.
//   - No dithering noise, no external binary dependency.

/* Map an (R,G,B) triple to its nearest xterm-256 palette index.
 * Strategy: try the 6×6×6 color cube (indices 16..231) and the
 * 24-step grayscale ramp (232..255), return whichever has the
 * smaller squared-distance match. 16 ANSI colors (0..15) are skipped
 * because their RGB values differ between terminals and using them
 * would make colors inconsistent across users. */
static inline int rgb_to_xterm256(int r, int g, int b) {
    static const int CUBE_LEVELS[6] = {0, 95, 135, 175, 215, 255};
    /* Find nearest cube level for each channel. */
    int rl = 0, gl = 0, bl = 0;
    int rd = 256*256, gd = 256*256, bd = 256*256;
    for (int i = 0; i < 6; i++) {
        int dr = r - CUBE_LEVELS[i]; dr *= dr;
        int dg = g - CUBE_LEVELS[i]; dg *= dg;
        int db = b - CUBE_LEVELS[i]; db *= db;
        if (dr < rd) { rd = dr; rl = i; }
        if (dg < gd) { gd = dg; gl = i; }
        if (db < bd) { bd = db; bl = i; }
    }
    int cube_idx = 16 + 36 * rl + 6 * gl + bl;
    int cube_r = CUBE_LEVELS[rl], cube_g = CUBE_LEVELS[gl], cube_b = CUBE_LEVELS[bl];
    int cube_dist = (r - cube_r) * (r - cube_r)
                  + (g - cube_g) * (g - cube_g)
                  + (b - cube_b) * (b - cube_b);
    /* Gray ramp: index 232 + i has value 8 + 10*i. Check only if
     * the color is approximately achromatic (R/G/B close to each other). */
    int max_rgb = r; if (g > max_rgb) max_rgb = g; if (b > max_rgb) max_rgb = b;
    int min_rgb = r; if (g < min_rgb) min_rgb = g; if (b < min_rgb) min_rgb = b;
    if (max_rgb - min_rgb < 24) {
        int avg = (r + g + b) / 3;
        int gray_i = (avg - 8 + 5) / 10;
        if (gray_i < 0) gray_i = 0;
        if (gray_i > 23) gray_i = 23;
        int gray_v = 8 + 10 * gray_i;
        int gray_dist = 3 * (avg - gray_v) * (avg - gray_v);
        if (gray_dist < cube_dist) return 232 + gray_i;
    }
    return cube_idx;
}

static inline void avg_region(
    const uint8_t *px_r, const uint8_t *px_g, const uint8_t *px_b, int stride,
    int x0, int y0, int x1, int y1,
    int *out_r, int *out_g, int *out_b, int *out_lum
) {
    long sr = 0, sg = 0, sb_ = 0;
    int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int o = y * stride + x;
            sr += px_r[o]; sg += px_g[o]; sb_ += px_b[o];
            n++;
        }
    }
    if (n == 0) n = 1;
    *out_r = (int)(sr / n);
    *out_g = (int)(sg / n);
    *out_b = (int)(sb_ / n);
    *out_lum = brightness((uint8_t)*out_r, (uint8_t)*out_g, (uint8_t)*out_b);
}

static void render_blocks_hq(void) {
    /* Use the bordered cropped viewport so edge cells are always black
     * (stable anchors for Ink). We read pixels out of the file we just
     * wrote so the sampling sees the border. Reading the framebuffer
     * directly would also work but then we'd need to duplicate the
     * cropping + border logic. */
    char ppm_path[700];
    snprintf(ppm_path, sizeof ppm_path, "%s/frame.ppm", STATE_DIR);
    int ppm_h = dump_ppm_viewport(ppm_path);
    if (!ppm_h) return;

    int sw = DOOMGENERIC_RESX, sh = ppm_h;

    /* Read the PPM body back into arrays (plain bytes for convenience). */
    FILE *p = fopen(ppm_path, "rb");
    if (!p) return;
    /* PPM P6 header is three newline-terminated lines:
     *   P6\n
     *   <width> <height>\n
     *   <maxval>\n
     * Then binary RGB data immediately follows. */
    int newlines = 0, c;
    while (newlines < 3 && (c = fgetc(p)) != EOF) {
        if (c == '\n') newlines++;
    }
    uint8_t *buf = malloc((size_t)sw * sh * 3);
    if (!buf) { fclose(p); return; }
    if (fread(buf, 1, (size_t)sw * sh * 3, p) != (size_t)sw * sh * 3) {
        free(buf); fclose(p); return;
    }
    fclose(p);

    /* Deinterleave so avg_region can index by channel. */
    uint8_t *R = malloc((size_t)sw * sh);
    uint8_t *G = malloc((size_t)sw * sh);
    uint8_t *B = malloc((size_t)sw * sh);
    if (!R || !G || !B) { free(buf); free(R); free(G); free(B); return; }
    for (int i = 0; i < sw * sh; i++) {
        R[i] = buf[i * 3 + 0];
        G[i] = buf[i * 3 + 1];
        B[i] = buf[i * 3 + 2];
    }
    free(buf);

    char tmp_path[700];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", FRAME_ANSI);
    FILE *fa = fopen(tmp_path, "w");
    if (!fa) { free(R); free(G); free(B); return; }

    /* HUD line */
    char hud[600];
    make_hud_line(hud, sizeof hud);
    fprintf(fa, "%s\n", hud);

    /* DIC_BLOCKS_COLORS=256 switches to xterm-256 palette output —
     * ~half the per-cell SGR bytes, much lower Ink diff-engine load
     * during whole-scene flashes (damage overlay, fireballs). Default
     * is TrueColor for fidelity. */
    const char *bc_env = getenv("DIC_BLOCKS_COLORS");
    int use_256 = (bc_env && !strcmp(bc_env, "256"));

    double cell_w = (double)sw / COLS;
    double cell_h = (double)sh / ROWS;

    for (int ry = 0; ry < ROWS; ry++) {
        for (int cx = 0; cx < COLS; cx++) {
            int r[4], g[4], bb[4], lum[4];
            /* 2×2 subpixel layout: 0=TL 1=TR 2=BL 3=BR */
            for (int i = 0; i < 4; i++) {
                int dx = i & 1, dy = (i >> 1) & 1;
                int x0 = (int)(cx * cell_w + dx * cell_w / 2.0);
                int x1 = (int)(cx * cell_w + (dx + 1) * cell_w / 2.0);
                int y0 = (int)(ry * cell_h + dy * cell_h / 2.0);
                int y1 = (int)(ry * cell_h + (dy + 1) * cell_h / 2.0);
                if (x1 > sw) x1 = sw;
                if (y1 > sh) y1 = sh;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                avg_region(R, G, B, sw, x0, y0, x1, y1,
                           &r[i], &g[i], &bb[i], &lum[i]);
            }
            uint8_t rr[4], gg[4], bb2[4];
            for (int i = 0; i < 4; i++) {
                rr[i] = (uint8_t)r[i];
                gg[i] = (uint8_t)g[i];
                bb2[i] = (uint8_t)bb[i];
            }
            int fR, fG, fB, bR, bG, bB;
            int pat = classify_cell(4, rr, gg, bb2, lum,
                                    &fR, &fG, &fB, &bR, &bG, &bB);
            /* Remap LSB-first pattern to QUAD_CHARS bit order
             * (TL=8 TR=4 BL=2 BR=1). */
            int q = 0;
            if (pat & 1) q |= 8;
            if (pat & 2) q |= 4;
            if (pat & 4) q |= 2;
            if (pat & 8) q |= 1;
            if (use_256) {
                fprintf(fa,
                    "\x1b[38;5;%03d;48;5;%03dm%s",
                    rgb_to_xterm256(fR, fG, fB),
                    rgb_to_xterm256(bR, bG, bB),
                    QUAD_CHARS[q]);
            } else {
                fprintf(fa,
                    "\x1b[38;2;%03d;%03d;%03dm\x1b[48;2;%03d;%03d;%03dm%s",
                    fR, fG, fB, bR, bG, bB, QUAD_CHARS[q]);
            }
        }
        fprintf(fa, "\x1b[0m\n");
    }
    fclose(fa);
    rename(tmp_path, FRAME_ANSI);

    free(R); free(G); free(B);

    FILE *ft = fopen(FRAME_TXT, "w");
    if (ft) { fprintf(ft, "DOOM  tick %d  blocks-hq %dx%d\n", gametic, COLS, ROWS); fclose(ft); }
}

static void render_ansi(FILE *fa, FILE *ft) {
    switch (RENDER) {
        case RENDER_SIXEL:    render_sixel(fa, ft);    break;
        case RENDER_QUADRANT: render_quadrant(fa, ft); break;
        default:              render_sextant(fa, ft);  break;
    }
}

static const char *WEAPON_NAMES[NUMWEAPONS] = {
    "fist", "pistol", "shotgun", "chaingun", "missile",
    "plasma", "bfg", "chainsaw", "supershotgun",
};
static const char *AMMO_NAMES[NUMAMMO] = {
    "bullets", "shells", "cells", "rockets",
};

static void write_state_json(void) {
    FILE *f = fopen(STATE_JSON, "w");
    if (!f) return;

    player_t *p = &players[consoleplayer];
    int w = (p->readyweapon >= 0 && p->readyweapon < NUMWEAPONS) ? p->readyweapon : 0;

    fprintf(f,
        "{\n"
        "  \"backend\": \"doomgeneric\",\n"
        "  \"tick\": %d,\n"
        "  \"episode\": %d,\n"
        "  \"map\": %d,\n"
        "  \"res\": { \"w\": %d, \"h\": %d },\n"
        "  \"ansi_cols\": %d,\n"
        "  \"ansi_rows\": %d,\n"
        "  \"hp\": %d,\n"
        "  \"armor\": %d,\n"
        "  \"armor_type\": %d,\n"
        "  \"weapon\": \"%s\",\n"
        "  \"weapons_owned\": [",
        gametic, gameepisode, gamemap,
        DOOMGENERIC_RESX, DOOMGENERIC_RESY, COLS, ROWS,
        p->health, p->armorpoints, p->armortype,
        WEAPON_NAMES[w]);

    int first = 1;
    for (int i = 0; i < NUMWEAPONS; i++) {
        if (p->weaponowned[i]) {
            fprintf(f, "%s\"%s\"", first ? "" : ", ", WEAPON_NAMES[i]);
            first = 0;
        }
    }

    fprintf(f, "],\n  \"ammo\": {");
    for (int i = 0; i < NUMAMMO; i++) {
        fprintf(f, "%s\"%s\": %d, \"max_%s\": %d",
                i ? ", " : " ",
                AMMO_NAMES[i], p->ammo[i],
                AMMO_NAMES[i], p->maxammo[i]);
    }
    fprintf(f, " },\n");

    if (p->mo) {
        fprintf(f,
            "  \"position\": { \"x\": %d, \"y\": %d, \"z\": %d, \"angle\": %u },\n",
            p->mo->x >> 16, p->mo->y >> 16, p->mo->z >> 16,
            (unsigned)(p->mo->angle));
    } else {
        fprintf(f, "  \"position\": null,\n");
    }

    fprintf(f,
        "  \"killcount\": %d,\n"
        "  \"itemcount\": %d,\n"
        "  \"secretcount\": %d,\n"
        "  \"cards\": { \"blue\": %d, \"yellow\": %d, \"red\": %d },\n"
        "  \"powers\": { \"invuln\": %d, \"strength\": %d, \"invis\": %d, \"radsuit\": %d, \"allmap\": %d, \"ironfeet\": %d },\n"
        "  \"message\": \"%s\"\n"
        "}\n",
        p->killcount, p->itemcount, p->secretcount,
        p->cards[it_bluecard]   || p->cards[it_blueskull],
        p->cards[it_yellowcard] || p->cards[it_yellowskull],
        p->cards[it_redcard]    || p->cards[it_redskull],
        p->powers[pw_invulnerability] > 0 ? p->powers[pw_invulnerability] : 0,
        p->powers[pw_strength]         > 0 ? p->powers[pw_strength]         : 0,
        p->powers[pw_invisibility]     > 0 ? p->powers[pw_invisibility]     : 0,
        p->powers[pw_ironfeet]         > 0 ? p->powers[pw_ironfeet]         : 0,
        p->powers[pw_allmap]           > 0 ? p->powers[pw_allmap]           : 0,
        p->powers[pw_ironfeet]         > 0 ? p->powers[pw_ironfeet]         : 0,
        p->message ? p->message : "");
    fclose(f);
}

void DG_DrawFrame(void) {
    uint64_t t = now_ms();
    if (t - last_frame_ms < FRAME_INTERVAL_MS) return;
    last_frame_ms = t;

    /* Renderers that write their own frame.ansi atomically (they need
     * to run a shell pipeline or do multi-pass work that the simple
     * render_ansi path can't express). All of them write the bordered
     * PPM at $STATE_DIR/frame.ppm so we can reuse it for frame.png. */
    if (RENDER == RENDER_CHAFA || RENDER == RENDER_TIMG || RENDER == RENDER_BLOCKS_HQ) {
        if      (RENDER == RENDER_CHAFA)     render_chafa_direct();
        else if (RENDER == RENDER_TIMG)      render_timg_direct();
        else                                 render_blocks_hq();
        write_state_json();
        char ppm[700];
        snprintf(ppm, sizeof ppm, "%s/frame.ppm", STATE_DIR);
        emit_png_if_due(ppm);
        return;
    }

    char tmpA[700], tmpT[700];
    snprintf(tmpA, sizeof tmpA, "%s.tmp", FRAME_ANSI);
    snprintf(tmpT, sizeof tmpT, "%s.tmp", FRAME_TXT);
    FILE *fa = fopen(tmpA, "w");
    FILE *ft = fopen(tmpT, "w");
    if (fa && ft) render_ansi(fa, ft);
    if (fa) fclose(fa);
    if (ft) fclose(ft);
    rename(tmpA, FRAME_ANSI);
    rename(tmpT, FRAME_TXT);
    emit_png_if_due(NULL);
    write_state_json();
}

// ─── Main entry ──────────────────────────────────────────────────────────

static void on_signal(int sig) {
    (void)sig;
    unlink(PID_FILE);
    _exit(0);
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
