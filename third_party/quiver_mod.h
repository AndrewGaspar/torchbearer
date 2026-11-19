/* quiver_mod.h - Quiver mod SDK: live options and metrics for mods running inside a game.
 *
 * Protocol 2 (design: Quiver's design/MOD-CONFIG.md section 5 and design/SIGHT-HUD.md
 * sections 3.6 and 3.14). Single header, C89/C99/C++, Windows only (MinGW and MSVC),
 * kernel32 only: no CRT calls, no allocation on the hot path, no locks on the hot path.
 *
 * Use it in exactly one .c/.cpp file of your DLL:
 *
 *     #define QUIVER_MOD_VERSION "v5"          optional: your mod's version, shown by Quiver
 *     #define QUIVER_MOD_IMPLEMENTATION
 *     #include "quiver_mod.h"
 *
 * and include it without QUIVER_MOD_IMPLEMENTATION anywhere else.
 *
 * Behaviour:
 *  - Not running under Quiver (no QUIVER_MODS_PIPE in the environment): quiver_open()
 *    returns a handle on which every call does nothing and returns at once. No thread,
 *    no pipe, no allocation.
 *  - Under Quiver: one background thread owns the pipe. It connects, says hello, resends
 *    your declarations, sends metric values at most 10 times a second, and reconnects
 *    quietly (with a backoff) whenever the pipe goes away. Game threads never touch the
 *    pipe: they write into fixed slots and queues the thread drains. When those are full,
 *    or the pipe is slow, data is dropped (and counted), never waited for.
 *  - Never returns NULL; every function accepts NULL and the no-op handle.
 *  - All strings are UTF-8. Invalid UTF-8 is replaced, text values are cut at 48 bytes.
 *
 * Threads: quiver_gauge/count/state/text/mark may be called from any thread at any rate.
 * Callbacks run on the SDK thread, or, with QUIVER_POLL, inside quiver_poll() on your
 * thread. quiver_open() is safe in DllMain(DLL_PROCESS_ATTACH); quiver_close() is not
 * meant for DllMain (at process exit, just don't call it).
 *
 * Licence: 0BSD. Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted. THE SOFTWARE IS PROVIDED "AS IS" AND THE
 * AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE.
 */
#ifndef QUIVER_MOD_H
#define QUIVER_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIVER_MOD_PROTOCOL 2
#define QUIVER_MOD_SDK "quiver_mod.h 2.0"

typedef struct quiver_mod quiver_mod; /* opaque */

/* ---- Options (MOD-CONFIG) ---------------------------------------------------------- */

typedef struct quiver_value {
    const char *id;   /* option id, e.g. "world.npc_density" */
    const char *text; /* as your file would hold it, e.g. "4" */
    const char *json; /* the typed value as JSON, e.g. "4", "\"d3d12\"", "false" */
} quiver_value;

/* A batch of values Quiver wants applied (someone changed them in the app, the CLI, an
 * agent or the in-game overlay). Strings are valid during the call only. Return 0 to
 * accept; non-zero rejects every value of the batch not otherwise named. Call
 * quiver_reject() first to refuse single values with a reason. Quiver reports a value as
 * applied live only after this returns and accepts it. */
typedef int (*quiver_on_values)(void *user, const quiver_value *values, int count);
/* Run an action ([action.<id>] in quiver-options.toml). `arg` may be NULL. */
typedef void (*quiver_on_action)(void *user, const char *action_id, const char *arg);

enum {
    QUIVER_POLL = 1u << 0,        /* callbacks run inside quiver_poll() on your thread
                                     (e.g. where you already reload your config);
                                     default: on the SDK thread */
    QUIVER_NO_SNAPSHOT = 1u << 1  /* don't deliver the values Quiver connects with as a
                                     callback (quiver_get() still knows them) */
};

enum { QUIVER_LOG_ERROR = 0, QUIVER_LOG_WARN = 1, QUIVER_LOG_INFO = 2, QUIVER_LOG_DEBUG = 3 };

/* Start talking to Quiver if the game runs under it. `surface` is your schema's surface id
 * ([a-z][a-z0-9_-], at most 48 bytes), which is also your metrics' namespace.
 * `schema_utf8` may be NULL (Quiver uses the quiver-options.toml your version ships). */
quiver_mod *quiver_open(const char *surface, const char *schema_utf8, unsigned flags);
/* 1 while connected and greeted by Quiver. */
int quiver_connected(const quiver_mod *m);

void quiver_on_values_cb(quiver_mod *m, quiver_on_values cb, void *user);
void quiver_on_action_cb(quiver_mod *m, quiver_on_action cb, void *user);
/* Declare an option you apply live (at least 1 per option id; up to 128). Quiver sends
 * `set` only for these; others are saved and apply when your mod next reads its file. */
int quiver_handle_option(quiver_mod *m, const char *id);
/* Inside a quiver_on_values callback: refuse one value, with a reason shown to the user. */
void quiver_reject(quiver_mod *m, const char *id, const char *reason);
/* The last value Quiver gave (at connect or since) or you reported with quiver_changed().
 * Copies its text into buf (always NUL-terminated); returns 1, or 0 if unknown. */
int quiver_get(quiver_mod *m, const char *id, char *buf, int buf_len);
/* You changed a value yourself. persist=1: from your own menu (saved to the profile);
 * persist=0: runtime state such as a hotkey flip (shown, not saved). */
void quiver_changed(quiver_mod *m, const char *id, const char *text, int persist);
/* Shortcut: declares (once) a `text` metric named `id` and sets it. */
void quiver_readout(quiver_mod *m, const char *id, const char *text);
/* A line in Quiver's session log (at most 10 a second get through). */
void quiver_log(quiver_mod *m, int level, const char *text);
/* QUIVER_POLL mode: deliver pending callbacks now. Cheap when there are none. */
void quiver_poll(quiver_mod *m);
/* Say bye and stop the SDK thread (waits up to a second). The handle is gone after. */
void quiver_close(quiver_mod *m);

/* ---- Metrics (SIGHT-HUD) ----------------------------------------------------------- */

typedef int32_t quiver_metric; /* small index; < 0 = invalid */
#define QUIVER_NO_METRIC (-1)

typedef enum quiver_kind {
    QUIVER_GAUGE = 1, /* a number, latest wins (fps, ms, %) */
    QUIVER_COUNTER,   /* monotonic count; Quiver shows a rate */
    QUIVER_SERIES,    /* per-event samples: needs a ring (not in this Quiver yet) */
    QUIVER_FRAME,     /* per-frame records: needs a ring (not in this Quiver yet) */
    QUIVER_STATE,     /* one of the descriptor's declared values ("on", "off") */
    QUIVER_TEXT,      /* short text ("DLSS K", "1280x800") */
    QUIVER_EVENT      /* marks on a timeline (quiver_mark) */
} quiver_kind;

/* Declare (or look up) a metric. `name` is relative to the surface: "fg.state" becomes
 * "aco-dlss.fg.state" (at most 64 bytes in all). `descriptor_json` may be NULL when
 * quiver-options.toml declares [metric.<name>]; otherwise a JSON object with the other
 * descriptor fields (quiver_perf::descriptor::MetricDescriptor: unit, label, short, help,
 * values, roles, thresholds...; not id, name or kind). A state metric needs its `values`.
 * Call at init, not per frame. Up to 64. Returns QUIVER_NO_METRIC on the no-op handle or
 * a bad name; Quiver's verdict comes later (quiver_metric_status). */
quiver_metric quiver_metric_declare(quiver_mod *m, const char *name, quiver_kind kind, const char *descriptor_json);
/* 1 accepted by Quiver, 0 not yet answered, -1 refused (or invalid). */
int quiver_metric_status(const quiver_mod *m, quiver_metric h);

/* Low-rate values: coalesced (latest wins) and sent at most 10 times a second. No locks,
 * no syscalls, no allocation. */
void quiver_gauge(quiver_mod *m, quiver_metric h, double value);
void quiver_count(quiver_mod *m, quiver_metric h, uint64_t delta);
void quiver_state(quiver_mod *m, quiver_metric h, const char *value); /* a declared value */
void quiver_text(quiver_mod *m, quiver_metric h, const char *utf8);   /* <= 48 bytes, copied */
/* A mark on an event metric (FG reset, teleport, benchmark begin). Times are QPC ticks
 * (quiver_now()); dur_qpc 0 = none. `label` may be NULL (<= 48 bytes). */
void quiver_mark(quiver_mod *m, quiver_metric h, int64_t t_qpc, int64_t dur_qpc, const char *label);
int64_t quiver_now(void); /* QueryPerformanceCounter */

/* Per-frame data (SIGHT-HUD phase 2: shared-memory rings, whose layout is quiver-perf's
 * include/quiver_perf.h; this header will include it then). Declared so mods can be written
 * against them now; until Quiver offers rings they drop everything (counted), and
 * quiver_perf_wanted() returns 0. */
enum {
    QUIVER_FRAME_REAL = 1u << 0,      /* rendered by the game */
    QUIVER_FRAME_GENERATED = 1u << 1, /* generated (interpolated/extrapolated) */
    QUIVER_FRAME_NOFG = 1u << 2       /* FG off: real and the only present */
};
typedef struct quiver_frame {
    uint64_t key;       /* your present sequence on this swapchain (1, 2, 3...) */
    uint32_t flags;     /* QUIVER_FRAME_* */
    uint32_t swapchain; /* any id; change it when the swapchain is recreated/resized */
    int64_t t;          /* the record's time (e.g. your Present call) */
    int nfields;        /* <= 12, in the order the descriptor's `fields` lists them */
    int64_t f[12];      /* timestamps (QPC) or integers; 0 = absent */
} quiver_frame;
void quiver_frame_submit(quiver_mod *m, quiver_metric h, const quiver_frame *fr);
void quiver_sample(quiver_mod *m, quiver_metric h, int64_t t_qpc, double value);
int quiver_perf_wanted(const quiver_mod *m);

/* ---- Diagnostics ------------------------------------------------------------------- */

typedef struct quiver_stats {
    unsigned long connects; /* pipe connections made */
    unsigned long sent;     /* messages written */
    unsigned long received; /* messages read */
    unsigned long dropped;  /* values, marks or messages dropped (full, slow, refused) */
} quiver_stats;
void quiver_get_stats(const quiver_mod *m, quiver_stats *out);

#ifdef __cplusplus
}
#endif
#endif /* QUIVER_MOD_H */

/* ===================================================================================== */
#ifdef QUIVER_MOD_IMPLEMENTATION
#ifndef QUIVER_MOD_IMPLEMENTED
#define QUIVER_MOD_IMPLEMENTED

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define QV_UNDEF_LEAN
#endif
#include <windows.h>
#ifdef QV_UNDEF_LEAN
#undef WIN32_LEAN_AND_MEAN
#undef QV_UNDEF_LEAN
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QV_MAX_METRICS 64
#define QV_MAX_OPTIONS 128
#define QV_MAX_VALUES 256
#define QV_ID 64           /* option ids and metric names */
#define QV_TEXT 48         /* text/state values, mark labels */
#define QV_MAX_FRAME 65536 /* what we send */
#define QV_MAX_RX 262144   /* what we accept */
#define QV_OUTQ 65536      /* queued control messages (changed, log, ack) */
#define QV_INQ 262144      /* QUIVER_POLL: received messages waiting for quiver_poll */
#define QV_MARKS 64
#define QV_REJ 4096
#define QV_READ 16384
#define QV_UNSENT (-1)
#define QV_REFUSED (-2)
#define QV_SENT (-3)

typedef struct qv_slot {
    char name[QV_ID + 1];
    int kind;
    char *desc;            /* inner members of the descriptor object, or NULL */
    volatile LONG remote;  /* Quiver's index; QV_UNSENT, QV_SENT (no answer yet), QV_REFUSED */
    volatile LONG dirty;   /* has a value Quiver hasn't got */
    volatile LONG has;     /* ever had a value (resent on reconnect) */
    volatile LONG lock;    /* try-lock for text */
    volatile LONG64 bits;  /* gauge: double bits; counter: total */
    char text[QV_TEXT + 1];
} qv_slot;

typedef struct qv_mark {
    volatile LONG state; /* 0 free, 1 being written, 2 ready */
    LONG slot;
    int64_t at, dur;
    int has_label;
    char label[QV_TEXT + 1];
} qv_mark;

typedef struct qv_kv {
    char id[QV_ID + 1];
    char *text;
} qv_kv;

typedef struct qv_buf {
    char *p;
    size_t len, cap, max;
    int over;
} qv_buf;

struct quiver_mod {
    int live; /* 0: the no-op handle */
    unsigned flags;
    char surface[49];
    char *schema;
    char module[128];
    WCHAR pipe[260];
    HANDLE thread, stop_ev, wake_ev, rev, wev, h;
    HMODULE self;
    CRITICAL_SECTION cs;
    volatile LONG connected;
    volatile LONG decl_dirty, opts_dirty;
    int caps_metrics, caps_options;
    quiver_on_values on_values;
    void *values_user;
    quiver_on_action on_action;
    void *action_user;
    qv_slot *slots;
    volatile LONG nslots;
    char (*opts)[QV_ID + 1];
    int nopts;
    qv_kv *vals;
    int nvals;
    qv_buf outq, inq;
    qv_mark *marks;
    volatile LONG mark_next;
    /* while a values callback runs */
    DWORD deliver_tid;
    qv_buf rej;
    /* the SDK thread's own */
    char *rx, *rtmp;
    size_t rx_len;
    char *wbuf;
    LARGE_INTEGER last_m, last_decl, freq;
    DWORD backoff;
    volatile LONG st_connects, st_sent, st_received, st_dropped;
};

static quiver_mod qv_noop_handle; /* zeroed: not live */

/* ---- small helpers (no CRT; volatile keeps compilers from turning the loops back into
 * memcpy/strlen calls) ---- */

static size_t qv_strlen(const char *s) {
    const volatile char *v = s;
    size_t n = 0;
    if (!s) return 0;
    while (v[n]) n++;
    return n;
}

static void qv_copy(void *dst, const void *src, size_t n) {
    volatile char *d = (volatile char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
}

static void qv_zero(void *dst, size_t n) {
    volatile char *d = (volatile char *)dst;
    while (n--) *d++ = 0;
}

static int qv_contains(const char *hay, size_t n, const char *needle) {
    size_t k = qv_strlen(needle), i, j;
    for (i = 0; i + k <= n; i++) {
        for (j = 0; j < k && hay[i + j] == needle[j]; j++) {}
        if (j == k) return 1;
    }
    return 0;
}

static int qv_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void *qv_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void qv_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static char *qv_strdup(const char *s, size_t max) {
    size_t n = qv_strlen(s);
    char *d;
    if (n > max) n = max;
    d = (char *)qv_alloc(n + 1);
    if (d) qv_copy(d, s, n);
    return d;
}

/* Length of the longest prefix of s (at most max bytes) that ends on a UTF-8 boundary. */
static size_t qv_cut(const char *s, size_t max) {
    size_t n = qv_strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) n--;
    return n;
}

/* [a-z0-9_.-], 1..max bytes. */
static int qv_ident(const char *s, size_t max, int dots) {
    size_t n = 0;
    if (!s || !*s) return 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= max) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (dots && c == '.') ||
              (dots == 2 && c >= 'A' && c <= 'Z')))
            return 0;
    }
    return 1;
}

static int qv_buf_init(qv_buf *b, size_t cap, size_t max) {
    b->p = (char *)qv_alloc(cap);
    b->len = 0;
    b->cap = b->p ? cap : 0;
    b->max = max;
    b->over = 0;
    return b->p != 0;
}

static void qv_buf_free(qv_buf *b) {
    qv_free(b->p);
    b->p = 0;
    b->len = b->cap = 0;
}

static void qv_put(qv_buf *b, const char *s, size_t n) {
    if (b->over) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *np;
        while (cap < b->len + n) cap *= 2;
        if (cap > b->max) cap = b->max;
        if (b->len + n > cap) { b->over = 1; return; }
        np = b->p ? (char *)HeapReAlloc(GetProcessHeap(), 0, b->p, cap) : (char *)qv_alloc(cap);
        if (!np) { b->over = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    qv_copy(b->p + b->len, s, n);
    b->len += n;
}

static void qv_puts(qv_buf *b, const char *s) { qv_put(b, s, qv_strlen(s)); }

static void qv_put_u64(qv_buf *b, uint64_t v) {
    char t[24];
    int i = 23;
    t[i] = 0;
    do { t[--i] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    qv_puts(b, t + i);
}

static void qv_put_i64(qv_buf *b, int64_t v) {
    if (v < 0) { qv_puts(b, "-"); qv_put_u64(b, (uint64_t)0 - (uint64_t)v); }
    else qv_put_u64(b, (uint64_t)v);
}

/* A finite double as JSON (6 significant decimals is plenty for a HUD). 0 if not finite. */
static int qv_put_double(qv_buf *b, double v) {
    uint64_t ip, fp;
    int e = 0, i;
    char frac[8];
    if (!(v == v) || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) return 0;
    if (v < 0) { qv_puts(b, "-"); v = -v; }
    if (v != 0 && (v >= 1e15 || v < 1e-4)) {
        while (v >= 10) { v /= 10; e++; }
        while (v < 1) { v *= 10; e--; }
    }
    ip = (uint64_t)v;
    fp = (uint64_t)((v - (double)ip) * 1e6 + 0.5);
    if (fp >= 1000000) { ip++; fp -= 1000000; }
    if (e && ip >= 10) { ip /= 10; e++; }
    qv_put_u64(b, ip);
    if (fp) {
        for (i = 5; i >= 0; i--) { frac[i] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        i = 6;
        while (i > 0 && frac[i - 1] == '0') i--;
        frac[i] = 0;
        qv_puts(b, ".");
        qv_puts(b, frac);
    }
    if (e) { qv_puts(b, "e"); qv_put_i64(b, e); }
    return 1;
}

/* A JSON string of at most `max` bytes of s, with invalid UTF-8 replaced by U+FFFD and
 * control characters escaped. */
static void qv_put_str(qv_buf *b, const char *s, size_t max) {
    static const char hexd[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    size_t n = qv_cut((const char *)p, max), i = 0;
    qv_puts(b, "\"");
    while (i < n) {
        unsigned char c = p[i];
        size_t len = 0, k;
        if (c < 0x80) {
            if (c == '"' || c == '\\') { char e[2]; e[0] = '\\'; e[1] = (char)c; qv_put(b, e, 2); }
            else if (c < 0x20) { char e[6]; e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = hexd[c >> 4]; e[5] = hexd[c & 15]; qv_put(b, e, 6); }
            else qv_put(b, (const char *)&p[i], 1);
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        for (k = 1; len && k < len; k++)
            if (i + k >= n || (p[i + k] & 0xC0) != 0x80) len = 0;
        /* overlong 3/4-byte forms, surrogates, > U+10FFFF */
        if (len == 3 && ((c == 0xE0 && p[i + 1] < 0xA0) || (c == 0xED && p[i + 1] >= 0xA0))) len = 0;
        if (len == 4 && ((c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] >= 0x90))) len = 0;
        if (!len) { qv_put(b, "\xEF\xBF\xBD", 3); i++; continue; }
        qv_put(b, (const char *)&p[i], len);
        i += len;
    }
    qv_puts(b, "\"");
}

/* ---- a small JSON reader for what Quiver sends ---- */

typedef struct qv_js {
    const char *p, *end;
} qv_js;

static void qv_ws(qv_js *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int qv_lit(qv_js *j, char c) {
    qv_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return 1; }
    return 0;
}

static int qv_hex4(const char *p, unsigned *out) {
    int i;
    unsigned v = 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parse a string; write at most cap-1 bytes (cut on a character) plus a NUL into out
 * (out may be NULL to skip). Returns 0 on malformed input. */
static int qv_str(qv_js *j, char *out, size_t cap) {
    size_t n = 0;
    qv_ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        char u[4];
        size_t ul = 1;
        unsigned cp;
        if (*j->p == '\\') {
            if (j->end - j->p < 2) return 0;
            j->p++;
            switch (*j->p) {
            case '"': u[0] = '"'; break;
            case '\\': u[0] = '\\'; break;
            case '/': u[0] = '/'; break;
            case 'b': u[0] = '\b'; break;
            case 'f': u[0] = '\f'; break;
            case 'n': u[0] = '\n'; break;
            case 'r': u[0] = '\r'; break;
            case 't': u[0] = '\t'; break;
            case 'u':
                if (j->end - j->p < 5 || !qv_hex4(j->p + 1, &cp)) return 0;
                j->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (j->end - j->p < 7 || j->p[1] != '\\' || j->p[2] != 'u' || !qv_hex4(j->p + 3, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        cp = 0xFFFD;
                    } else {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                if (cp < 0x80) { u[0] = (char)cp; }
                else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); ul = 2; }
                else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); ul = 3; }
                else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); ul = 4; }
                break;
            default: return 0;
            }
            j->p++;
        } else {
            u[0] = *j->p++;
        }
        if (out && n + ul < cap) { qv_copy(out + n, u, ul); n += ul; }
        else if (out) { cap = n + 1; } /* full: stop writing (keeps a whole character) */
    }
    if (j->p >= j->end) return 0;
    j->p++;
    if (out && cap) out[n] = 0;
    return 1;
}

static int qv_skip(qv_js *j, int depth) {
    qv_ws(j);
    if (j->p >= j->end || depth > 64) return 0;
    switch (*j->p) {
    case '"': return qv_str(j, 0, 0);
    case '{':
    case '[': {
        char close = *j->p == '{' ? '}' : ']';
        int obj = close == '}';
        j->p++;
        if (qv_lit(j, close)) return 1;
        for (;;) {
            if (obj && (!qv_str(j, 0, 0) || !qv_lit(j, ':'))) return 0;
            if (!qv_skip(j, depth + 1)) return 0;
            if (qv_lit(j, ',')) continue;
            return qv_lit(j, close);
        }
    }
    default:
        if (*j->p != '-' && *j->p != 't' && *j->p != 'f' && *j->p != 'n' && (*j->p < '0' || *j->p > '9')) return 0;
        while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n' && *j->p != '\r' && *j->p != '\t') j->p++;
        return 1;
    }
}

static int qv_int(qv_js *j, int64_t *out) {
    int64_t v = 0;
    int neg = 0, any = 0;
    qv_ws(j);
    if (j->p < j->end && *j->p == '-') { neg = 1; j->p++; }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (v < 922337203685477580LL) v = v * 10 + (*j->p - '0');
        j->p++;
        any = 1;
    }
    if (!any) return 0;
    /* tolerate a fraction or exponent: skip it */
    while (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-' || (*j->p >= '0' && *j->p <= '9'))) j->p++;
    *out = neg ? -v : v;
    return 1;
}

/* Object iteration: after qv_obj_begin, each qv_obj_next reads a key and leaves j at its
 * value (which the caller must consume). Returns 1 for a member, 0 at the end, -1 bad. */
static int qv_obj_begin(qv_js *j) { return qv_lit(j, '{'); }

static int qv_obj_next(qv_js *j, int *first, char *key, size_t cap) {
    if (qv_lit(j, '}')) return 0;
    if (!*first && !qv_lit(j, ',')) return -1;
    *first = 0;
    if (!qv_str(j, key, cap) || !qv_lit(j, ':')) return -1;
    return 1;
}

/* ---- queues ---- */

static void qv_queue(quiver_mod *m, qv_buf *q, const char *body, size_t n) {
    char len[4];
    EnterCriticalSection(&m->cs);
    if (q->len + 4 + n > q->max) {
        InterlockedIncrement(&m->st_dropped);
    } else {
        len[0] = (char)(n & 0xFF); len[1] = (char)((n >> 8) & 0xFF); len[2] = (char)((n >> 16) & 0xFF); len[3] = (char)((n >> 24) & 0xFF);
        q->over = 0;
        qv_put(q, len, 4);
        qv_put(q, body, n);
        if (q->over) { q->over = 0; InterlockedIncrement(&m->st_dropped); }
    }
    LeaveCriticalSection(&m->cs);
}

/* Move a queue's contents out (the caller frees them). */
static char *qv_take(quiver_mod *m, qv_buf *q, size_t *len) {
    char *out = 0;
    EnterCriticalSection(&m->cs);
    if (q->len) {
        out = (char *)qv_alloc(q->len);
        if (out) { qv_copy(out, q->p, q->len); *len = q->len; }
        q->len = 0;
    }
    LeaveCriticalSection(&m->cs);
    return out;
}

static void qv_control(quiver_mod *m, qv_buf *b) {
    if (!b->over && b->len) {
        qv_queue(m, &m->outq, b->p, b->len);
        SetEvent(m->wake_ev);
    } else if (b->over) {
        InterlockedIncrement(&m->st_dropped);
    }
}

/* ---- the values cache ---- */

static void qv_cache_set(quiver_mod *m, const char *id, const char *text) {
    int i;
    char *t = qv_strdup(text, 4096);
    if (!t) return;
    EnterCriticalSection(&m->cs);
    for (i = 0; i < m->nvals; i++) {
        if (qv_streq(m->vals[i].id, id)) {
            qv_free(m->vals[i].text);
            m->vals[i].text = t;
            t = 0;
            break;
        }
    }
    if (t && m->nvals < QV_MAX_VALUES) {
        qv_copy(m->vals[m->nvals].id, id, qv_cut(id, QV_ID));
        m->vals[m->nvals].text = t;
        m->nvals++;
        t = 0;
    }
    LeaveCriticalSection(&m->cs);
    qv_free(t);
}

/* ---- the SDK thread ---- */

static int qv_write(quiver_mod *m, const char *body, size_t n) {
    OVERLAPPED ov;
    DWORD done = 0;
    BOOL ok;
    if (n > QV_MAX_FRAME) { InterlockedIncrement(&m->st_dropped); return 1; }
    m->wbuf[0] = (char)(n & 0xFF); m->wbuf[1] = (char)((n >> 8) & 0xFF); m->wbuf[2] = (char)((n >> 16) & 0xFF); m->wbuf[3] = (char)((n >> 24) & 0xFF);
    qv_copy(m->wbuf + 4, body, n);
    qv_zero(&ov, sizeof ov);
    ov.hEvent = m->wev;
    ResetEvent(m->wev);
    ok = WriteFile(m->h, m->wbuf, (DWORD)(n + 4), &done, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        /* Backpressure: Quiver isn't reading. Don't queue behind it; reconnect later. */
        if (WaitForSingleObject(m->wev, 500) != WAIT_OBJECT_0) {
            CancelIo(m->h);
            GetOverlappedResult(m->h, &ov, &done, TRUE);
            return 0;
        }
        ok = GetOverlappedResult(m->h, &ov, &done, FALSE);
    }
    if (!ok || done != n + 4) return 0;
    InterlockedIncrement(&m->st_sent);
    return 1;
}

static int qv_send(quiver_mod *m, qv_buf *b) {
    if (b->over) { InterlockedIncrement(&m->st_dropped); return 1; }
    return qv_write(m, b->p, b->len);
}

static void qv_hello(quiver_mod *m, qv_buf *b) {
    b->len = 0;
    b->over = 0;
    qv_puts(b, "{\"t\":\"hello\",\"proto\":2,\"sdk\":\"" QUIVER_MOD_SDK "\",\"mod\":");
    qv_put_str(b, m->surface, 48);
    qv_puts(b, ",\"pid\":");
    qv_put_u64(b, GetCurrentProcessId());
    if (m->module[0]) { qv_puts(b, ",\"module\":"); qv_put_str(b, m->module, 127); }
#ifdef QUIVER_MOD_VERSION
    qv_puts(b, ",\"version\":");
    qv_put_str(b, QUIVER_MOD_VERSION, 32);
#endif
    if (m->schema) { qv_puts(b, ",\"schema\":"); qv_put_str(b, m->schema, QV_MAX_FRAME / 2); }
    qv_puts(b, ",\"caps\":[\"options\",\"metrics\",\"actions\",\"log\"]}");
}

/* Deliver a values object (`welcome`/`set`) to the callback; returns the ack's rejected
 * members in m->rej. `seq` < 0: no ack. */
static void qv_deliver_values(quiver_mod *m, const char *vstart, const char *vend, int64_t seq, int callback) {
    qv_js j;
    int first = 1, count = 0, r, i, cap;
    size_t arena_len = (size_t)(vend - vstart) * 3 + 64;
    char *arena, *ap;
    quiver_value *vals;
    char key[QV_ID + 1];
    quiver_on_values cb;
    void *user;
    qv_buf ack;

    cap = 256;
    arena = (char *)qv_alloc(arena_len + sizeof(quiver_value) * (size_t)cap);
    if (!arena) return;
    vals = (quiver_value *)arena;
    ap = arena + sizeof(quiver_value) * (size_t)cap;
    j.p = vstart;
    j.end = vend;
    if (!qv_obj_begin(&j)) { qv_free(arena); return; }
    while ((r = qv_obj_next(&j, &first, key, sizeof key)) == 1) {
        int f2 = 1, r2;
        char k2[8];
        char *id = ap, *text = 0, *json = 0;
        size_t kl = qv_strlen(key);
        qv_copy(ap, key, kl + 1);
        ap += kl + 1;
        if (!qv_obj_begin(&j)) { r = -1; break; }
        while ((r2 = qv_obj_next(&j, &f2, k2, sizeof k2)) == 1) {
            if (qv_streq(k2, "text")) {
                text = ap;
                if (!qv_str(&j, ap, (size_t)(arena + arena_len + sizeof(quiver_value) * (size_t)cap - ap))) { r2 = -1; break; }
                ap += qv_strlen(ap) + 1;
            } else if (qv_streq(k2, "json")) {
                const char *s;
                qv_ws(&j);
                s = j.p;
                if (!qv_skip(&j, 1)) { r2 = -1; break; }
                json = ap;
                qv_copy(ap, s, (size_t)(j.p - s));
                ap += j.p - s;
                *ap++ = 0;
            } else if (!qv_skip(&j, 1)) {
                r2 = -1;
                break;
            }
        }
        if (r2 < 0) { r = -1; break; }
        if (!text || !qv_ident(id, QV_ID, 2)) continue;
        qv_cache_set(m, id, text);
        if (count < cap) {
            vals[count].id = id;
            vals[count].text = text;
            vals[count].json = json ? json : "null";
            count++;
        }
    }
    EnterCriticalSection(&m->cs);
    cb = m->on_values;
    user = m->values_user;
    LeaveCriticalSection(&m->cs);
    m->rej.len = 0;
    m->rej.over = 0;
    if (r == 0 && callback && cb && count) {
        m->deliver_tid = GetCurrentThreadId();
        if (cb(user, vals, count) != 0) {
            for (i = 0; i < count; i++) quiver_reject(m, vals[i].id, "rejected by the mod");
        }
        m->deliver_tid = 0;
    }
    if (seq >= 0 && qv_buf_init(&ack, 256, QV_MAX_FRAME)) {
        qv_puts(&ack, "{\"t\":\"ack\",\"seq\":");
        qv_put_i64(&ack, seq);
        if (m->rej.len) { qv_puts(&ack, ",\"rejected\":{"); qv_put(&ack, m->rej.p, m->rej.len); qv_puts(&ack, "}"); }
        qv_puts(&ack, "}");
        qv_control(m, &ack);
        qv_buf_free(&ack);
    }
    m->rej.len = 0;
    qv_free(arena);
}

/* Find a member's value span in a top-level object. */
static int qv_member(const char *body, size_t n, const char *name, const char **vs, const char **ve) {
    qv_js j;
    int first = 1, r;
    char key[16];
    j.p = body;
    j.end = body + n;
    if (!qv_obj_begin(&j)) return 0;
    while ((r = qv_obj_next(&j, &first, key, sizeof key)) == 1) {
        qv_ws(&j);
        *vs = j.p;
        if (!qv_skip(&j, 1)) return 0;
        *ve = j.p;
        if (qv_streq(key, name)) return 1;
    }
    return 0;
}

static int qv_member_str(const char *body, size_t n, const char *name, char *out, size_t cap) {
    const char *vs, *ve;
    qv_js j;
    if (!qv_member(body, n, name, &vs, &ve)) return 0;
    j.p = vs;
    j.end = ve;
    return qv_str(&j, out, cap);
}

static int qv_member_int(const char *body, size_t n, const char *name, int64_t *out) {
    const char *vs, *ve;
    qv_js j;
    if (!qv_member(body, n, name, &vs, &ve)) return 0;
    j.p = vs;
    j.end = ve;
    return qv_int(&j, out);
}

static int qv_has_cap(const char *body, size_t n, const char *cap) {
    const char *vs, *ve;
    qv_js j;
    char c[16];
    if (!qv_member(body, n, "caps", &vs, &ve)) return 0;
    j.p = vs;
    j.end = ve;
    if (!qv_lit(&j, '[')) return 0;
    if (qv_lit(&j, ']')) return 0;
    do {
        if (!qv_str(&j, c, sizeof c)) return 0;
        if (qv_streq(c, cap)) return 1;
    } while (qv_lit(&j, ','));
    return 0;
}

/* `id` is slot i's full id, "<surface>.<name>". */
static int qv_is_slot(const quiver_mod *m, int i, const char *id) {
    const char *a = m->surface;
    while (*a && *a == *id) { a++; id++; }
    return !*a && *id == '.' && qv_streq(m->slots[i].name, id + 1);
}

static void qv_metrics_ok(quiver_mod *m, const char *body, size_t n) {
    const char *vs, *ve;
    qv_js j;
    int first, r, i, nslots = (int)m->nslots;
    char name[QV_ID + 1];
    int64_t idx;
    if (qv_member(body, n, "ids", &vs, &ve)) {
        j.p = vs; j.end = ve; first = 1;
        if (qv_obj_begin(&j))
            while ((r = qv_obj_next(&j, &first, name, sizeof name)) == 1) {
                if (!qv_int(&j, &idx)) break;
                for (i = 0; i < nslots; i++)
                    if (qv_is_slot(m, i, name) && idx >= 0 && idx < 0xFFFF) InterlockedExchange(&m->slots[i].remote, (LONG)idx);
            }
    }
    if (qv_member(body, n, "rejected", &vs, &ve)) {
        char why[256];
        j.p = vs; j.end = ve; first = 1;
        if (qv_obj_begin(&j))
            while ((r = qv_obj_next(&j, &first, name, sizeof name)) == 1) {
                int retry;
                if (!qv_str(&j, why, sizeof why)) break;
                /* "too many declarations; retry in a second": send it again later */
                retry = qv_contains(why, qv_strlen(why), "retry");
                for (i = 0; i < nslots; i++)
                    if (qv_is_slot(m, i, name)) InterlockedExchange(&m->slots[i].remote, retry ? QV_UNSENT : QV_REFUSED);
                if (retry) InterlockedExchange(&m->decl_dirty, 1);
            }
    }
}

static void qv_action(quiver_mod *m, const char *body, size_t n) {
    char id[QV_ID + 1], arg[1024];
    int has_arg;
    quiver_on_action cb;
    void *user;
    if (!qv_member_str(body, n, "id", id, sizeof id)) return;
    has_arg = qv_member_str(body, n, "arg", arg, sizeof arg);
    EnterCriticalSection(&m->cs);
    cb = m->on_action;
    user = m->action_user;
    LeaveCriticalSection(&m->cs);
    if (cb) cb(user, id, has_arg ? arg : 0);
}

/* Handle one message from Quiver on the thread that delivers callbacks. */
static void qv_dispatch(quiver_mod *m, const char *body, size_t n) {
    char t[16];
    const char *vs, *ve;
    int64_t seq;
    if (!qv_member_str(body, n, "t", t, sizeof t)) return;
    if (qv_streq(t, "welcome")) {
        if (qv_member(body, n, "values", &vs, &ve)) qv_deliver_values(m, vs, ve, -1, !(m->flags & QUIVER_NO_SNAPSHOT));
    } else if (qv_streq(t, "set")) {
        if (qv_member_int(body, n, "seq", &seq) && qv_member(body, n, "values", &vs, &ve)) qv_deliver_values(m, vs, ve, seq, 1);
    } else if (qv_streq(t, "action")) {
        qv_action(m, body, n);
    }
}

/* A message arrived on the SDK thread. Returns 0 to disconnect. */
static int qv_received(quiver_mod *m, const char *body, size_t n) {
    char t[16];
    InterlockedIncrement(&m->st_received);
    if (!qv_member_str(body, n, "t", t, sizeof t)) return 1;
    if (qv_streq(t, "welcome")) {
        m->caps_metrics = qv_has_cap(body, n, "metrics");
        m->caps_options = qv_has_cap(body, n, "options");
        InterlockedExchange(&m->connected, 1);
        m->backoff = 200;
    } else if (qv_streq(t, "metrics_ok")) {
        qv_metrics_ok(m, body, n);
        return 1;
    } else if (qv_streq(t, "bye")) {
        /* Quiver refused us (or is going away): come back much later. */
        m->backoff = 30000;
        return 0;
    } else if (!qv_streq(t, "set") && !qv_streq(t, "action")) {
        return 1; /* perf_ctl, ring, newer messages: not used by this SDK */
    }
    if (m->flags & QUIVER_POLL) {
        qv_queue(m, &m->inq, body, n);
        if (qv_streq(t, "welcome")) {
            /* the welcome snapshot is queued like a set; nothing else to do now */
        }
    } else {
        qv_dispatch(m, body, n);
    }
    return 1;
}

static int qv_flush(quiver_mod *m, qv_buf *b, int force_m) {
    int i, nslots = (int)m->nslots, any = 0, sent_marks = 0;
    LARGE_INTEGER now;
    char *q;
    size_t qlen = 0, off;
    if (!m->connected) return 1;
    /* options this mod applies */
    if (InterlockedExchange(&m->opts_dirty, 0) && m->caps_options) {
        b->len = 0; b->over = 0;
        qv_puts(b, "{\"t\":\"options\",\"ids\":[");
        EnterCriticalSection(&m->cs);
        for (i = 0; i < m->nopts; i++) { if (i) qv_puts(b, ","); qv_put_str(b, m->opts[i], QV_ID); }
        LeaveCriticalSection(&m->cs);
        qv_puts(b, "]}");
        if (!qv_send(m, b)) return 0;
    }
    /* declarations not sent yet, batched and at most 4 a second (Quiver's limit) */
    QueryPerformanceCounter(&now);
    if (m->decl_dirty && m->caps_metrics && now.QuadPart - m->last_decl.QuadPart >= m->freq.QuadPart / 4 &&
        InterlockedExchange(&m->decl_dirty, 0)) {
        static const char *kinds[] = {"", "gauge", "counter", "series", "frame", "state", "text", "event"};
        b->len = 0; b->over = 0;
        qv_puts(b, "{\"t\":\"metrics\",\"defs\":[");
        for (i = 0; i < nslots; i++) {
            qv_slot *s = &m->slots[i];
            if (s->remote != QV_UNSENT) continue;
            InterlockedExchange(&s->remote, QV_SENT);
            if (any++) qv_puts(b, ",");
            /* the full id: surface and name are plain identifiers, nothing to escape */
            qv_puts(b, "{\"id\":\"");
            qv_puts(b, m->surface);
            qv_puts(b, ".");
            qv_puts(b, s->name);
            qv_puts(b, "\"");
            qv_puts(b, ",\"kind\":\"");
            qv_puts(b, kinds[s->kind]);
            qv_puts(b, "\"");
            if (s->desc) { qv_puts(b, ","); qv_puts(b, s->desc); }
            qv_puts(b, "}");
        }
        qv_puts(b, "]}");
        m->last_decl = now;
        if (any && !qv_send(m, b)) return 0;
    }
    /* control messages (changed, log, ack) */
    q = qv_take(m, &m->outq, &qlen);
    for (off = 0; q && off + 4 <= qlen;) {
        size_t n = (size_t)(unsigned char)q[off] | ((size_t)(unsigned char)q[off + 1] << 8) | ((size_t)(unsigned char)q[off + 2] << 16) | ((size_t)(unsigned char)q[off + 3] << 24);
        if (off + 4 + n > qlen) break;
        if (!qv_write(m, q + off + 4, n)) { qv_free(q); return 0; }
        off += 4 + n;
    }
    qv_free(q);
    if (!m->caps_metrics) return 1;
    /* marks (at most 2 per flush: 20 a second) */
    for (i = 0; i < QV_MARKS && sent_marks < 2; i++) {
        qv_mark *k = &m->marks[i];
        LONG r;
        if (k->state != 2) continue;
        r = m->slots[k->slot].remote;
        if (r == QV_UNSENT || r == QV_SENT) continue;
        if (r >= 0) {
            b->len = 0; b->over = 0;
            qv_puts(b, "{\"t\":\"mark\",\"index\":");
            qv_put_i64(b, r);
            qv_puts(b, ",\"ts\":");
            qv_put_i64(b, k->at);
            if (k->dur > 0) { qv_puts(b, ",\"dur\":"); qv_put_i64(b, k->dur); }
            if (k->has_label) { qv_puts(b, ",\"label\":"); qv_put_str(b, k->label, QV_TEXT); }
            qv_puts(b, "}");
            sent_marks++;
            InterlockedExchange(&k->state, 0);
            if (!qv_send(m, b)) return 0;
        } else {
            InterlockedExchange(&k->state, 0);
            InterlockedIncrement(&m->st_dropped);
        }
    }
    /* coalesced values, at most every 100 ms */
    QueryPerformanceCounter(&now);
    if (!force_m && (now.QuadPart - m->last_m.QuadPart) < m->freq.QuadPart / 10) return 1;
    b->len = 0; b->over = 0;
    qv_puts(b, "{\"t\":\"m\",\"ts\":");
    qv_put_i64(b, now.QuadPart);
    qv_puts(b, ",\"v\":{");
    any = 0;
    for (i = 0; i < nslots; i++) {
        qv_slot *s = &m->slots[i];
        LONG r = s->remote;
        size_t mark = b->len;
        if (!s->dirty || r == QV_UNSENT || r == QV_SENT) continue;
        if (r < 0) { InterlockedExchange(&s->dirty, 0); continue; }
        if (s->kind == QUIVER_TEXT || s->kind == QUIVER_STATE) {
            char tmp[QV_TEXT + 1];
            if (InterlockedCompareExchange(&s->lock, 1, 0) != 0) continue; /* being written: next time */
            InterlockedExchange(&s->dirty, 0);
            qv_copy(tmp, s->text, sizeof tmp);
            InterlockedExchange(&s->lock, 0);
            if (any) qv_puts(b, ",");
            qv_puts(b, "\"");
            qv_put_i64(b, r);
            qv_puts(b, "\":");
            qv_put_str(b, tmp, QV_TEXT);
        } else {
            LONG64 bits;
            InterlockedExchange(&s->dirty, 0);
            bits = InterlockedCompareExchange64(&s->bits, 0, 0);
            if (any) qv_puts(b, ",");
            qv_puts(b, "\"");
            qv_put_i64(b, r);
            qv_puts(b, "\":");
            if (s->kind == QUIVER_COUNTER) {
                qv_put_u64(b, (uint64_t)bits);
            } else {
                double d;
                qv_copy(&d, &bits, sizeof d);
                if (!qv_put_double(b, d)) { b->len = mark; continue; }
            }
        }
        any = 1;
    }
    qv_puts(b, "}}");
    if (any) {
        m->last_m = now;
        if (!qv_send(m, b)) return 0;
    }
    return 1;
}

/* One connection, until it fails or we stop. */
static void qv_session(quiver_mod *m) {
    OVERLAPPED rov;
    HANDLE evs[3];
    char *tmp = m->rtmp;
    int pending = 0, i, nslots;
    DWORD got;
    qv_buf b;
    if (!qv_buf_init(&b, 4096, QV_MAX_FRAME)) return;
    m->rx_len = 0;
    m->caps_metrics = m->caps_options = 0;
    /* Quiver forgets everything per connection: declare again, resend values. */
    nslots = (int)m->nslots;
    for (i = 0; i < nslots; i++) {
        if (m->slots[i].remote != QV_REFUSED) InterlockedExchange(&m->slots[i].remote, QV_UNSENT);
        if (m->slots[i].has) InterlockedExchange(&m->slots[i].dirty, 1);
    }
    InterlockedExchange(&m->decl_dirty, 1);
    InterlockedExchange(&m->opts_dirty, 1);
    qv_hello(m, &b);
    if (!qv_send(m, &b)) goto out;
    evs[0] = m->stop_ev;
    evs[1] = m->rev;
    evs[2] = m->wake_ev;
    for (;;) {
        DWORD w;
        if (!pending) {
            qv_zero(&rov, sizeof rov);
            rov.hEvent = m->rev;
            ResetEvent(m->rev);
            if (ReadFile(m->h, tmp, QV_READ, &got, &rov)) {
                SetEvent(m->rev); /* completed at once: handle below */
            } else if (GetLastError() != ERROR_IO_PENDING) {
                goto out;
            }
            pending = 1;
        }
        w = WaitForMultipleObjects(3, evs, FALSE, 100);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            size_t off = 0;
            if (!GetOverlappedResult(m->h, &rov, &got, FALSE)) { pending = 0; goto out; }
            pending = 0;
            if (m->rx_len + got > QV_MAX_RX + 4) goto out;
            qv_copy(m->rx + m->rx_len, tmp, got);
            m->rx_len += got;
            while (m->rx_len - off >= 4) {
                const unsigned char *p = (const unsigned char *)m->rx + off;
                size_t n = (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
                if (n > QV_MAX_RX) goto out;
                if (m->rx_len - off - 4 < n) break;
                if (!qv_received(m, m->rx + off + 4, n)) goto out;
                off += 4 + n;
            }
            if (off) {
                size_t k;
                for (k = 0; k + off < m->rx_len; k++) m->rx[k] = m->rx[k + off];
                m->rx_len -= off;
            }
        }
        if (!qv_flush(m, &b, 0)) goto out;
    }
    /* stopping: send what's left and say bye (best effort) */
    qv_flush(m, &b, 1);
    b.len = 0; b.over = 0;
    qv_puts(&b, "{\"t\":\"bye\"}");
    qv_send(m, &b);
out:
    InterlockedExchange(&m->connected, 0);
    if (pending) {
        CancelIo(m->h);
        GetOverlappedResult(m->h, &rov, &got, TRUE);
    }
    qv_buf_free(&b);
}

static DWORD WINAPI qv_thread(LPVOID arg) {
    quiver_mod *m = (quiver_mod *)arg;
    HMODULE self = m->self;
    for (;;) {
        DWORD wait;
        m->h = CreateFileW(m->pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (m->h != INVALID_HANDLE_VALUE) {
            InterlockedIncrement(&m->st_connects);
            qv_session(m);
            CloseHandle(m->h);
            m->h = INVALID_HANDLE_VALUE;
        }
        wait = m->backoff;
        if (m->backoff < 10000) m->backoff *= 2;
        if (m->backoff > 10000 && wait < 30000) m->backoff = 10000;
        if (WaitForSingleObject(m->stop_ev, wait) == WAIT_OBJECT_0) break;
    }
    /* The thread holds a reference on the module, so the DLL can't vanish under it. */
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

/* ---- public API ---- */

quiver_mod *quiver_open(const char *surface, const char *schema_utf8, unsigned flags) {
    quiver_mod *m;
    WCHAR wmod[MAX_PATH];
    DWORD n;
    HMODULE self = 0;
    int i;
    static const WCHAR prefix[] = {'\\', '\\', '.', '\\', 'p', 'i', 'p', 'e', '\\', 0};
    if (!qv_ident(surface, 48, 0) || surface[0] < 'a' || surface[0] > 'z') return &qv_noop_handle;
    m = (quiver_mod *)qv_alloc(sizeof *m);
    if (!m) return &qv_noop_handle;
    n = GetEnvironmentVariableW(L"QUIVER_MODS_PIPE", m->pipe, 260);
    for (i = 0; prefix[i] && n; i++)
        if (m->pipe[i] != prefix[i]) n = 0;
    if (n == 0 || n >= 260 || n <= 9) { qv_free(m); return &qv_noop_handle; }
    m->live = 1;
    m->flags = flags;
    qv_copy(m->surface, surface, qv_strlen(surface));
    m->backoff = 200;
    m->h = INVALID_HANDLE_VALUE;
    QueryPerformanceFrequency(&m->freq);
    InitializeCriticalSection(&m->cs);
    m->slots = (qv_slot *)qv_alloc(sizeof(qv_slot) * QV_MAX_METRICS);
    m->opts = (char(*)[QV_ID + 1])qv_alloc((QV_ID + 1) * QV_MAX_OPTIONS);
    m->vals = (qv_kv *)qv_alloc(sizeof(qv_kv) * QV_MAX_VALUES);
    m->marks = (qv_mark *)qv_alloc(sizeof(qv_mark) * QV_MARKS);
    m->rx = (char *)qv_alloc(QV_MAX_RX + 4 + QV_READ);
    m->rtmp = (char *)qv_alloc(QV_READ);
    m->wbuf = (char *)qv_alloc(QV_MAX_FRAME + 4);
    qv_buf_init(&m->outq, 1024, QV_OUTQ);
    qv_buf_init(&m->inq, 1024, QV_INQ);
    qv_buf_init(&m->rej, 256, QV_REJ);
    if (schema_utf8) m->schema = qv_strdup(schema_utf8, QV_MAX_FRAME / 2);
    /* Name the module for Quiver's status list, and keep it loaded while the thread runs. */
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(const void *)&qv_noop_handle, &self)) {
        n = GetModuleFileNameW(self, wmod, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            DWORD start = n;
            while (start > 0 && wmod[start - 1] != '\\' && wmod[start - 1] != '/') start--;
            WideCharToMultiByte(CP_UTF8, 0, wmod + start, (int)(n - start), m->module, (int)sizeof m->module - 1, NULL, NULL);
        }
    }
    m->self = self;
    m->stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    m->wake_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    m->rev = CreateEventW(NULL, TRUE, FALSE, NULL);
    m->wev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!m->slots || !m->opts || !m->vals || !m->marks || !m->rx || !m->rtmp || !m->wbuf || !m->outq.p || !m->inq.p || !m->rej.p || !m->stop_ev ||
        !m->wake_ev || !m->rev || !m->wev)
        goto fail;
    /* CreateThread is fine in DllMain: the thread starts once the loader lock is free, and
     * nothing here waits for it. */
    m->thread = CreateThread(NULL, 0, qv_thread, m, 0, NULL);
    if (!m->thread) goto fail;
    return m;
fail:
    if (self) FreeLibrary(self);
    if (m->stop_ev) CloseHandle(m->stop_ev);
    if (m->wake_ev) CloseHandle(m->wake_ev);
    if (m->rev) CloseHandle(m->rev);
    if (m->wev) CloseHandle(m->wev);
    DeleteCriticalSection(&m->cs);
    qv_free(m->slots); qv_free(m->opts); qv_free(m->vals); qv_free(m->marks); qv_free(m->rx); qv_free(m->rtmp); qv_free(m->wbuf);
    qv_buf_free(&m->outq); qv_buf_free(&m->inq); qv_buf_free(&m->rej);
    qv_free(m->schema);
    qv_free(m);
    return &qv_noop_handle;
}

#define QV_LIVE(m) ((m) && (m)->live)

int quiver_connected(const quiver_mod *m) { return QV_LIVE(m) && m->connected ? 1 : 0; }

void quiver_on_values_cb(quiver_mod *m, quiver_on_values cb, void *user) {
    if (!QV_LIVE(m)) return;
    EnterCriticalSection(&m->cs);
    m->on_values = cb;
    m->values_user = user;
    LeaveCriticalSection(&m->cs);
}

void quiver_on_action_cb(quiver_mod *m, quiver_on_action cb, void *user) {
    if (!QV_LIVE(m)) return;
    EnterCriticalSection(&m->cs);
    m->on_action = cb;
    m->action_user = user;
    LeaveCriticalSection(&m->cs);
}

int quiver_handle_option(quiver_mod *m, const char *id) {
    int i, ok = 0;
    if (!QV_LIVE(m) || !qv_ident(id, QV_ID, 2)) return 0;
    EnterCriticalSection(&m->cs);
    for (i = 0; i < m->nopts; i++)
        if (qv_streq(m->opts[i], id)) ok = 1;
    if (!ok && m->nopts < QV_MAX_OPTIONS) {
        qv_copy(m->opts[m->nopts++], id, qv_strlen(id));
        ok = 1;
    }
    LeaveCriticalSection(&m->cs);
    InterlockedExchange(&m->opts_dirty, 1);
    SetEvent(m->wake_ev);
    return ok;
}

void quiver_reject(quiver_mod *m, const char *id, const char *reason) {
    qv_buf key;
    if (!QV_LIVE(m) || !qv_ident(id, QV_ID, 2) || m->deliver_tid != GetCurrentThreadId()) return;
    if (qv_buf_init(&key, 80, 80)) {
        int dup;
        qv_put_str(&key, id, QV_ID);
        qv_puts(&key, ":");
        qv_put(&key, "", 1); /* NUL */
        dup = !key.over && qv_contains(m->rej.p, m->rej.len, key.p);
        qv_buf_free(&key);
        if (dup) return;
    }
    if (m->rej.len) qv_puts(&m->rej, ",");
    qv_put_str(&m->rej, id, QV_ID);
    qv_puts(&m->rej, ":");
    qv_put_str(&m->rej, reason ? reason : "rejected by the mod", 200);
}

int quiver_get(quiver_mod *m, const char *id, char *buf, int buf_len) {
    int i, found = 0;
    if (buf && buf_len > 0) buf[0] = 0;
    if (!QV_LIVE(m) || !id || !buf || buf_len <= 0) return 0;
    EnterCriticalSection(&m->cs);
    for (i = 0; i < m->nvals; i++) {
        if (qv_streq(m->vals[i].id, id)) {
            size_t n = qv_cut(m->vals[i].text, (size_t)buf_len - 1);
            qv_copy(buf, m->vals[i].text, n);
            buf[n] = 0;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&m->cs);
    return found;
}

void quiver_changed(quiver_mod *m, const char *id, const char *text, int persist) {
    qv_buf b;
    if (!QV_LIVE(m) || !qv_ident(id, QV_ID, 2) || !text) return;
    qv_cache_set(m, id, text);
    if (!qv_buf_init(&b, 256, QV_MAX_FRAME)) return;
    qv_puts(&b, "{\"t\":\"changed\",\"values\":{");
    qv_put_str(&b, id, QV_ID);
    qv_puts(&b, ":");
    qv_put_str(&b, text, 4096);
    qv_puts(&b, persist ? "},\"persist\":true}" : "},\"persist\":false}");
    qv_control(m, &b);
    qv_buf_free(&b);
}

void quiver_log(quiver_mod *m, int level, const char *text) {
    qv_buf b;
    if (!QV_LIVE(m) || !text) return;
    if (!qv_buf_init(&b, 256, QV_MAX_FRAME)) return;
    qv_puts(&b, "{\"t\":\"log\",\"level\":");
    qv_put_i64(&b, level < 0 ? 0 : level > 3 ? 3 : level);
    qv_puts(&b, ",\"text\":");
    qv_put_str(&b, text, 1024);
    qv_puts(&b, "}");
    qv_control(m, &b);
    qv_buf_free(&b);
}

void quiver_poll(quiver_mod *m) {
    char *q;
    size_t len = 0, off;
    if (!QV_LIVE(m) || !(m->flags & QUIVER_POLL) || !m->inq.len) return;
    q = qv_take(m, &m->inq, &len);
    for (off = 0; q && off + 4 <= len;) {
        size_t n = (size_t)(unsigned char)q[off] | ((size_t)(unsigned char)q[off + 1] << 8) | ((size_t)(unsigned char)q[off + 2] << 16) | ((size_t)(unsigned char)q[off + 3] << 24);
        if (off + 4 + n > len) break;
        qv_dispatch(m, q + off + 4, n);
        off += 4 + n;
    }
    qv_free(q);
}

void quiver_close(quiver_mod *m) {
    int i;
    if (!QV_LIVE(m)) return;
    SetEvent(m->stop_ev);
    if (WaitForSingleObject(m->thread, 1000) != WAIT_OBJECT_0) {
        /* The thread is stuck (e.g. called under the loader lock): leave it its memory. */
        return;
    }
    CloseHandle(m->thread);
    CloseHandle(m->stop_ev);
    CloseHandle(m->wake_ev);
    CloseHandle(m->rev);
    CloseHandle(m->wev);
    DeleteCriticalSection(&m->cs);
    for (i = 0; i < (int)m->nslots; i++) qv_free(m->slots[i].desc);
    for (i = 0; i < m->nvals; i++) qv_free(m->vals[i].text);
    qv_free(m->slots); qv_free(m->opts); qv_free(m->vals); qv_free(m->marks); qv_free(m->rx); qv_free(m->rtmp); qv_free(m->wbuf);
    qv_buf_free(&m->outq); qv_buf_free(&m->inq); qv_buf_free(&m->rej);
    qv_free(m->schema);
    qv_free(m);
}

/* The members of a descriptor object, without its braces, if it is one object and doesn't
 * set id, name or kind itself. */
static char *qv_desc_members(const char *json) {
    qv_js j;
    int first = 1, r;
    char key[32];
    const char *open, *close;
    size_t n = qv_strlen(json);
    if (n > 8192) return 0;
    j.p = json;
    j.end = json + n;
    qv_ws(&j);
    open = j.p;
    if (!qv_obj_begin(&j)) return 0;
    while ((r = qv_obj_next(&j, &first, key, sizeof key)) == 1) {
        if (qv_streq(key, "id") || qv_streq(key, "name") || qv_streq(key, "kind")) return 0;
        if (!qv_skip(&j, 1)) return 0;
    }
    if (r < 0) return 0;
    close = j.p - 1;
    qv_ws(&j);
    if (j.p != j.end) return 0;
    open++;
    while (open < close && (*open == ' ' || *open == '\n' || *open == '\r' || *open == '\t')) open++;
    if (open == close) return 0;
    {
        char *d = (char *)qv_alloc((size_t)(close - open) + 1);
        if (d) qv_copy(d, open, (size_t)(close - open));
        return d;
    }
}

quiver_metric quiver_metric_declare(quiver_mod *m, const char *name, quiver_kind kind, const char *descriptor_json) {
    int i, found = -1;
    char *desc = 0;
    if (!QV_LIVE(m) || !qv_ident(name, QV_ID, 1) || (int)kind < QUIVER_GAUGE || (int)kind > QUIVER_EVENT) return QUIVER_NO_METRIC;
    /* the full id (surface.name) is at most 64 bytes */
    if (qv_strlen(m->surface) + 1 + qv_strlen(name) > QV_ID) return QUIVER_NO_METRIC;
    if (descriptor_json && !(desc = qv_desc_members(descriptor_json))) {
        /* an empty object is fine; anything else malformed is refused here */
        qv_js j;
        j.p = descriptor_json;
        j.end = descriptor_json + qv_strlen(descriptor_json);
        if (!(qv_lit(&j, '{') && qv_lit(&j, '}'))) return QUIVER_NO_METRIC;
    }
    EnterCriticalSection(&m->cs);
    for (i = 0; i < (int)m->nslots; i++)
        if (qv_streq(m->slots[i].name, name)) found = i;
    if (found >= 0) {
        LeaveCriticalSection(&m->cs);
        qv_free(desc);
        return m->slots[found].kind == (int)kind ? found : QUIVER_NO_METRIC;
    }
    if (m->nslots >= QV_MAX_METRICS) {
        LeaveCriticalSection(&m->cs);
        qv_free(desc);
        return QUIVER_NO_METRIC;
    }
    i = (int)m->nslots;
    qv_copy(m->slots[i].name, name, qv_strlen(name));
    m->slots[i].kind = (int)kind;
    m->slots[i].desc = desc;
    m->slots[i].remote = QV_UNSENT;
    InterlockedExchange(&m->nslots, i + 1); /* publish after the slot is filled */
    LeaveCriticalSection(&m->cs);
    InterlockedExchange(&m->decl_dirty, 1);
    SetEvent(m->wake_ev);
    return i;
}

int quiver_metric_status(const quiver_mod *m, quiver_metric h) {
    LONG r;
    if (!QV_LIVE(m) || h < 0 || h >= m->nslots) return -1;
    r = m->slots[h].remote;
    return r >= 0 ? 1 : r == QV_REFUSED ? -1 : 0;
}

static qv_slot *qv_slot_of(quiver_mod *m, quiver_metric h, int kind) {
    qv_slot *s;
    if (!QV_LIVE(m) || h < 0 || h >= m->nslots) return 0;
    s = &m->slots[h];
    return s->kind == kind ? s : 0;
}

void quiver_gauge(quiver_mod *m, quiver_metric h, double value) {
    qv_slot *s = qv_slot_of(m, h, QUIVER_GAUGE);
    LONG64 bits;
    if (!s) return;
    qv_copy(&bits, &value, sizeof bits);
    InterlockedExchange64(&s->bits, bits);
    s->has = 1;
    InterlockedExchange(&s->dirty, 1);
}

void quiver_count(quiver_mod *m, quiver_metric h, uint64_t delta) {
    qv_slot *s = qv_slot_of(m, h, QUIVER_COUNTER);
    if (!s) return;
    InterlockedExchangeAdd64(&s->bits, (LONG64)delta);
    s->has = 1;
    InterlockedExchange(&s->dirty, 1);
}

static void qv_set_text(quiver_mod *m, quiver_metric h, int kind, const char *utf8) {
    qv_slot *s = qv_slot_of(m, h, kind);
    size_t n;
    if (!s) return;
    if (InterlockedCompareExchange(&s->lock, 1, 0) != 0) { InterlockedIncrement(&m->st_dropped); return; }
    n = qv_cut(utf8 ? utf8 : "", QV_TEXT);
    qv_copy(s->text, utf8 ? utf8 : "", n);
    s->text[n] = 0;
    s->has = 1;
    InterlockedExchange(&s->dirty, 1);
    InterlockedExchange(&s->lock, 0);
}

void quiver_state(quiver_mod *m, quiver_metric h, const char *value) { qv_set_text(m, h, QUIVER_STATE, value); }
void quiver_text(quiver_mod *m, quiver_metric h, const char *utf8) { qv_set_text(m, h, QUIVER_TEXT, utf8); }

void quiver_readout(quiver_mod *m, const char *id, const char *text) {
    quiver_metric h;
    if (!QV_LIVE(m)) return;
    h = quiver_metric_declare(m, id, QUIVER_TEXT, 0);
    quiver_text(m, h, text);
}

void quiver_mark(quiver_mod *m, quiver_metric h, int64_t t_qpc, int64_t dur_qpc, const char *label) {
    qv_slot *s = qv_slot_of(m, h, QUIVER_EVENT);
    int i;
    LONG start;
    if (!s) return;
    start = InterlockedIncrement(&m->mark_next);
    for (i = 0; i < QV_MARKS; i++) {
        qv_mark *k = &m->marks[(start + i) & (QV_MARKS - 1)];
        if (InterlockedCompareExchange(&k->state, 1, 0) == 0) {
            size_t n = qv_cut(label ? label : "", QV_TEXT);
            k->slot = h;
            k->at = t_qpc;
            k->dur = dur_qpc;
            k->has_label = label != 0;
            qv_copy(k->label, label ? label : "", n);
            k->label[n] = 0;
            InterlockedExchange(&k->state, 2);
            return;
        }
    }
    InterlockedIncrement(&m->st_dropped);
}

int64_t quiver_now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

void quiver_frame_submit(quiver_mod *m, quiver_metric h, const quiver_frame *fr) {
    (void)h;
    (void)fr;
    if (QV_LIVE(m)) InterlockedIncrement(&m->st_dropped);
}

void quiver_sample(quiver_mod *m, quiver_metric h, int64_t t_qpc, double value) {
    (void)h;
    (void)t_qpc;
    (void)value;
    if (QV_LIVE(m)) InterlockedIncrement(&m->st_dropped);
}

int quiver_perf_wanted(const quiver_mod *m) {
    (void)m;
    return 0;
}

void quiver_get_stats(const quiver_mod *m, quiver_stats *out) {
    if (!out) return;
    qv_zero(out, sizeof *out);
    if (!QV_LIVE(m)) return;
    out->connects = (unsigned long)m->st_connects;
    out->sent = (unsigned long)m->st_sent;
    out->received = (unsigned long)m->st_received;
    out->dropped = (unsigned long)m->st_dropped;
}

#ifdef __cplusplus
}
#endif
#endif /* QUIVER_MOD_IMPLEMENTED */
#endif /* QUIVER_MOD_IMPLEMENTATION */
