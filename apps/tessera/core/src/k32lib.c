#include "k32lib.h"

#if defined(K32_NATIVE)
k32_prof_t k32_prof;
#endif

/* ---- memory primitives ---------------------------------------------------
 * Word-wide inner loops: under the wasm3 interpreter every loop iteration
 * costs the same whether it moves one byte or four, and wasm permits
 * unaligned i32 loads/stores (the aligned(1) typedef makes that explicit for
 * the native build too). No bulk-memory opcodes are used: they are not
 * proven on the boards. */
typedef u32 __attribute__((aligned(1), may_alias)) u32u;

void* memcpy(void* dst, const void* src, size_t n) {
    u8* d = (u8*)dst; const u8* s = (const u8*)src;
    while (n >= 4) { *(u32u*)d = *(const u32u*)s; d += 4; s += 4; n -= 4; }
    while (n--) *d++ = *s++;
    return dst;
}
void* memmove(void* dst, const void* src, size_t n) {
    u8* d = (u8*)dst; const u8* s = (const u8*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n >= 4) { *(u32u*)d = *(const u32u*)s; d += 4; s += 4; n -= 4; }
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n >= 4) { d -= 4; s -= 4; *(u32u*)d = *(const u32u*)s; n -= 4; }
        while (n--) *--d = *--s;
    }
    return dst;
}
void* memset(void* dst, int c, size_t n) {
    u8* d = (u8*)dst;
    u32 w = (u32)(u8)c; w |= w << 8; w |= w << 16;
    while (n >= 4) { *(u32u*)d = w; d += 4; n -= 4; }
    while (n--) *d++ = (u8)c;
    return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const u8* x = (const u8*)a; const u8* y = (const u8*)b;
    for (size_t i = 0; i < n; i++) { if (x[i] != y[i]) return (int)x[i] - (int)y[i]; }
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---- strings ------------------------------------------------------------- */
int k32_streq(const char* a, u32 alen, const char* lit) {
    u32 n = (u32)strlen(lit);
    return alen == n && memcmp(a, lit, n) == 0;
}
int k32_strneq(const char* a, u32 alen, const char* b, u32 blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}
int k32_strcmp(const char* a, u32 alen, const char* b, u32 blen) {
    u32 n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c) return c;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}
int k32_starts_with(const char* s, u32 len, const char* prefix, u32 plen) {
    return len >= plen && memcmp(s, prefix, plen) == 0;
}
u32 k32_fmt_u32(char* out, u32 cap, u32 v) {
    char tmp[12]; u32 n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    if (n > cap) return 0;
    for (u32 i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}
u32 k32_fmt_i64(char* out, u32 cap, i64 v) {
    char tmp[24]; u32 n = 0; int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (n + (u32)neg > cap) return 0;
    u32 o = 0;
    if (neg) out[o++] = '-';
    for (u32 i = 0; i < n; i++) out[o++] = tmp[n - 1 - i];
    return o;
}
u32 k32_fmt_hex32(char* out, u32 cap, u32 v) {
    static const char hx[] = "0123456789abcdef";
    if (cap < 8) return 0;
    for (int i = 7; i >= 0; i--) { out[i] = hx[v & 15]; v >>= 4; }
    return 8;
}
int k32_parse_i64(const char* s, u32 len, i64* out) {
    u32 i = 0; int neg = 0; i64 v = 0;
    if (len == 0) return 0;
    if (s[0] == '-') { neg = 1; i = 1; } else if (s[0] == '+') { i = 1; }
    if (i >= len) return 0;
    if (len - i > 18) return 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}
int k32_parse_u32(const char* s, u32 len, u32* out) {
    i64 v;
    if (!k32_parse_i64(s, len, &v) || v < 0 || v > 0xFFFFFFFFll) return 0;
    *out = (u32)v;
    return 1;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
u32 k32_url_decode(const char* in, u32 len, char* out, u32 cap) {
    u32 o = 0;
    for (u32 i = 0; i < len && o < cap; i++) {
        char c = in[i];
        if (c == '+') { out[o++] = ' '; }
        else if (c == '%' && i + 2 < len) {
            int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
            if (h >= 0 && l >= 0) { out[o++] = (char)((h << 4) | l); i += 2; }
            else out[o++] = c;
        } else out[o++] = c;
    }
    return o;
}
int k32_query_get(const char* q, u32 qlen, const char* key, const char** val, u32* vlen) {
    u32 klen = (u32)strlen(key);
    u32 i = 0;
    while (i < qlen) {
        u32 start = i;
        while (i < qlen && q[i] != '&') i++;
        u32 end = i;
        u32 eq = start;
        while (eq < end && q[eq] != '=') eq++;
        if (eq - start == klen && memcmp(q + start, key, klen) == 0) {
            if (eq < end) { *val = q + eq + 1; *vlen = end - eq - 1; }
            else { *val = q + end; *vlen = 0; }
            return 1;
        }
        i = end + 1;
    }
    return 0;
}
u32 k32_utf8_backoff(const char* s, u32 len) {
    /* Drop an incomplete trailing multi-byte sequence. */
    if (len == 0) return 0;
    u32 i = len;
    u32 cont = 0;
    while (i > 0 && ((u8)s[i - 1] & 0xC0) == 0x80) { i--; cont++; if (cont > 3) break; }
    if (i == 0) return len; /* all continuation bytes: leave as is */
    u8 lead = (u8)s[i - 1];
    u32 need = 0;
    if ((lead & 0x80) == 0) return len;
    if ((lead & 0xE0) == 0xC0) need = 1;
    else if ((lead & 0xF0) == 0xE0) need = 2;
    else if ((lead & 0xF8) == 0xF0) need = 3;
    else return len;
    if (cont >= need) return len;
    return i - 1;
}

/* ---- tokenizer ----------------------------------------------------------- */
void k32_tok_init(k32_tok* t, const char* s, u32 len) { t->s = s; t->len = len; t->pos = 0; }
static int is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
u32 k32_tok_next(k32_tok* t, char* out, u32 cap) {
    while (t->pos < t->len && !is_alnum(t->s[t->pos])) t->pos++;
    if (t->pos >= t->len) return 0;
    u32 n = 0;
    while (t->pos < t->len && is_alnum(t->s[t->pos])) {
        char c = t->s[t->pos++];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (n < cap) out[n++] = c;
    }
    return n;
}
static const char* const k_stop[] = {
    "the","a","an","of","to","in","on","is","and","with","for","when","it","at","by",
    "be","as","or","this","that","from","after","we","i","are","was","but","not","so",
    "if","its","into","than","then","also","do","does","did","has","have","had","will",
    "can","our","you","they","them","their","there","up","out","no","yes","about","over",
    "show","shows","looks","within","minute","seen","observed","while","testing","think",
    "please","verify","cannot","got","needs","exactly","one","two","first","same","got",
    "under","until","every","once","per","via","all","any","some","still","just","only",
    0
};
u32 k32_hash(const char* s, u32 len) {
    u32 h = 2166136261u;                 /* FNV-1a */
    for (u32 i = 0; i < len; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h;
}
/* The stopword list is hashed into a 256-slot open-addressing table on first
 * use (slot = list index + 1), so a lookup costs one or two string compares
 * instead of a scan over the whole list. */
#define K32_STOP_SLOTS 256u
static u8 k_stop_tab[K32_STOP_SLOTS];
static u8 k_stop_ready;
static void stop_build(void) {
    for (u32 i = 0; k_stop[i]; i++) {
        u32 h = k32_hash(k_stop[i], (u32)strlen(k_stop[i])) & (K32_STOP_SLOTS - 1);
        while (k_stop_tab[h]) h = (h + 1) & (K32_STOP_SLOTS - 1);
        k_stop_tab[h] = (u8)(i + 1);
    }
    k_stop_ready = 1;
}
int k32_is_stopword(const char* s, u32 len) {
    if (len < 2) return 1;
    if (!k_stop_ready) stop_build();
    u32 h = k32_hash(s, len) & (K32_STOP_SLOTS - 1);
    while (k_stop_tab[h]) {
        K32_PROF(stop_cmps);
        if (k32_streq(s, len, k_stop[k_stop_tab[h] - 1])) return 1;
        h = (h + 1) & (K32_STOP_SLOTS - 1);
    }
    return 0;
}

/* ---- PRNG ---------------------------------------------------------------- */
void k32_rng_seed(k32_rng* r, u32 seed) { r->s = seed ? seed : 0x9E3779B9u; }
u32 k32_rng_next(k32_rng* r) {
    u32 x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; r->s = x; return x;
}
u32 k32_rng_range(k32_rng* r, u32 n) { return n ? k32_rng_next(r) % n : 0; }

/* ---- arena --------------------------------------------------------------- */
void k32_arena_init(k32_arena* a, u8* base, u32 size) { a->base = base; a->size = size; a->top = 0; a->limit = size; }
void* k32_arena_alloc(k32_arena* a, u32 size, u32 align) {
    if (align == 0) align = 1;
    u32 top = (a->top + (align - 1)) & ~(align - 1);
    if (top < a->top) return 0;
    if (size > a->limit || top > a->limit - size) return 0;
    u8* p = a->base + top;
    a->top = top + size;
    memset(p, 0, size);
    return p;
}
void* k32_arena_alloc_top(k32_arena* a, u32 size, u32 align) {
    if (align == 0) align = 1;
    if (size > a->limit) return 0;
    u32 nl = (a->limit - size) & ~(align - 1);
    if (nl < a->top) return 0;
    a->limit = nl;
    return a->base + nl;          /* not zeroed: the caller owns every byte */
}
u32 k32_arena_free(const k32_arena* a) { return a->limit - a->top; }
u32 k32_arena_used(const k32_arena* a) { return a->top + (a->size - a->limit); }

/* ---- sorting ------------------------------------------------------------- */
static void sift_down(u16* a, u32 start, u32 end, k32_cmp_u16 cmp, void* ctx) {
    u32 root = start;
    for (;;) {
        u32 child = root * 2 + 1;
        if (child >= end) return;
        if (child + 1 < end && cmp(a[child], a[child + 1], ctx) < 0) child++;
        if (cmp(a[root], a[child], ctx) < 0) {
            u16 t = a[root]; a[root] = a[child]; a[child] = t;
            root = child;
        } else return;
    }
}
void k32_heapsort_u16(u16* a, u32 n, k32_cmp_u16 cmp, void* ctx) {
    if (n < 2) return;
    for (u32 i = n / 2; i > 0; i--) sift_down(a, i - 1, n, cmp, ctx);
    for (u32 end = n - 1; end > 0; end--) {
        u16 t = a[0]; a[0] = a[end]; a[end] = t;
        sift_down(a, 0, end, cmp, ctx);
    }
}
/* Bounded top-k: a[0..n) is kept sorted ascending under cmp; a candidate is
 * compared against the current k-th best first (one comparator call) and
 * only binary-searched + shifted in when it beats it. With a strict total
 * order the result is exactly the first k elements of a full sort. */
void k32_topk_init(k32_topk* t, u16* buf, u32 k, k32_cmp_u16 cmp, void* ctx) {
    t->a = buf; t->n = 0; t->k = k; t->cmp = cmp; t->ctx = ctx;
}
void k32_topk_push(k32_topk* t, u16 v) {
    if (t->k == 0) return;
    if (t->n == t->k) {
        if (t->cmp(v, t->a[t->n - 1], t->ctx) >= 0) return;   /* does not beat the k-th best */
        t->n--;
    }
    u32 lo = 0, hi = t->n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (t->cmp(t->a[mid], v, t->ctx) < 0) lo = mid + 1; else hi = mid;
    }
    if (lo < t->n) memmove(&t->a[lo + 1], &t->a[lo], (t->n - lo) * sizeof(u16));
    t->a[lo] = v;
    t->n++;
}
void k32_sort_u32(u32* a, u32 n) {
    /* shell sort, fine for the small arrays we use (<= a few thousand) */
    static const u32 gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (u32 g = 0; g < 8; g++) {
        u32 gap = gaps[g];
        if (gap >= n) continue;
        for (u32 i = gap; i < n; i++) {
            u32 v = a[i]; u32 j = i;
            while (j >= gap && a[j - gap] > v) { a[j] = a[j - gap]; j -= gap; }
            a[j] = v;
        }
    }
}

/* ---- JSON writer --------------------------------------------------------- */
void k32_jw_init(k32_jw* w, char* buf, u32 cap, k32_jw_flush_fn flush, void* ctx) {
    w->buf = buf; w->cap = cap; w->len = 0; w->flush = flush; w->ctx = ctx;
    w->depth = 0; w->after_key = 0; w->truncated = 0; w->total = 0;
    w->first[0] = 1;
}
void k32_jw_flush(k32_jw* w) {
    if (w->len && w->flush) { w->flush(w->ctx, w->buf, w->len); w->len = 0; }
}
static void jw_putc(k32_jw* w, char c) {
    if (w->len >= w->cap) {
        if (w->flush) k32_jw_flush(w);
        else { w->truncated = 1; return; }
    }
    w->buf[w->len++] = c; w->total++;
}
void k32_jw_raw(k32_jw* w, const char* s, u32 len) {
    for (u32 i = 0; i < len; i++) jw_putc(w, s[i]);
}
void k32_jw_lit(k32_jw* w, const char* s) { k32_jw_raw(w, s, (u32)strlen(s)); }
static void jw_sep(k32_jw* w) {
    if (w->after_key) { w->after_key = 0; return; }
    if (!w->first[w->depth]) jw_putc(w, ',');
    w->first[w->depth] = 0;
}
static void jw_open(k32_jw* w, char c) {
    jw_sep(w); jw_putc(w, c);
    if (w->depth < 15) w->depth++;
    w->first[w->depth] = 1;
}
static void jw_close(k32_jw* w, char c) {
    if (w->depth > 0) w->depth--;
    jw_putc(w, c);
}
void k32_jw_obj_begin(k32_jw* w) { jw_open(w, '{'); }
void k32_jw_obj_end(k32_jw* w)   { jw_close(w, '}'); }
void k32_jw_arr_begin(k32_jw* w) { jw_open(w, '['); }
void k32_jw_arr_end(k32_jw* w)   { jw_close(w, ']'); }
void k32_jw_escaped(k32_jw* w, const char* s, u32 len) {
    static const char hx[] = "0123456789abcdef";
    for (u32 i = 0; i < len; i++) {
        u8 c = (u8)s[i];
        switch (c) {
            case '"':  jw_putc(w, '\\'); jw_putc(w, '"'); break;
            case '\\': jw_putc(w, '\\'); jw_putc(w, '\\'); break;
            case '\n': jw_putc(w, '\\'); jw_putc(w, 'n'); break;
            case '\r': jw_putc(w, '\\'); jw_putc(w, 'r'); break;
            case '\t': jw_putc(w, '\\'); jw_putc(w, 't'); break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    jw_putc(w, '\\'); jw_putc(w, 'u'); jw_putc(w, '0'); jw_putc(w, '0');
                    jw_putc(w, hx[c >> 4]); jw_putc(w, hx[c & 15]);
                } else jw_putc(w, (char)c);
        }
    }
}
void k32_jw_key(k32_jw* w, const char* key) {
    jw_sep(w);
    jw_putc(w, '"'); k32_jw_escaped(w, key, (u32)strlen(key)); jw_putc(w, '"'); jw_putc(w, ':');
    w->after_key = 1;
}
void k32_jw_str(k32_jw* w, const char* s, u32 len) {
    jw_sep(w); jw_putc(w, '"'); k32_jw_escaped(w, s, len); jw_putc(w, '"');
}
static u32 esc_len(u8 c) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') return 2;
    if (c < 0x20 || c == 0x7f) return 6;
    return 1;
}
void k32_jw_str_max(k32_jw* w, const char* s, u32 len, u32 max_escaped) {
    /* write at most max_escaped bytes of escaped output, never splitting a UTF-8 sequence */
    u32 used = 0, end = 0;
    while (end < len) {
        u32 e = esc_len((u8)s[end]);
        if (used + e > max_escaped) break;
        used += e; end++;
    }
    if (end < len) end = k32_utf8_backoff(s, end);
    k32_jw_str(w, s, end);
}
void k32_jw_cstr(k32_jw* w, const char* s) { k32_jw_str(w, s, (u32)strlen(s)); }
void k32_jw_int(k32_jw* w, i64 v) {
    char tmp[24]; u32 n = k32_fmt_i64(tmp, sizeof tmp, v);
    jw_sep(w); k32_jw_raw(w, tmp, n);
}
void k32_jw_bool(k32_jw* w, int v) { jw_sep(w); k32_jw_lit(w, v ? "true" : "false"); }
void k32_jw_null(k32_jw* w) { jw_sep(w); k32_jw_lit(w, "null"); }

/* ---- JSON reader --------------------------------------------------------- */
typedef struct { char* b; u32 len; u32 pos; } jr;
static void jr_ws(jr* r) {
    while (r->pos < r->len) {
        char c = r->b[r->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r->pos++; else break;
    }
}
static void put_utf8(char* out, u32* o, u32 cp) {
    if (cp < 0x80) out[(*o)++] = (char)cp;
    else if (cp < 0x800) { out[(*o)++] = (char)(0xC0 | (cp >> 6)); out[(*o)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out[(*o)++] = (char)(0xE0 | (cp >> 12)); out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*o)++] = (char)(0x80 | (cp & 0x3F)); }
    else { out[(*o)++] = (char)(0xF0 | (cp >> 18)); out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[(*o)++] = (char)(0x80 | (cp & 0x3F)); }
}
/* Parses a string starting at the opening quote; unescapes in place. Returns 1 on success. */
static int jr_string(jr* r, char** s, u32* slen) {
    if (r->pos >= r->len || r->b[r->pos] != '"') return 0;
    r->pos++;
    char* out = r->b + r->pos; u32 o = 0;
    while (r->pos < r->len) {
        char c = r->b[r->pos];
        if (c == '"') { r->pos++; *s = out; *slen = o; return 1; }
        if ((u8)c < 0x20) return 0;
        if (c == '\\') {
            if (r->pos + 1 >= r->len) return 0;
            char e = r->b[r->pos + 1];
            r->pos += 2;
            switch (e) {
                case '"': out[o++] = '"'; break;
                case '\\': out[o++] = '\\'; break;
                case '/': out[o++] = '/'; break;
                case 'b': out[o++] = '\b'; break;
                case 'f': out[o++] = '\f'; break;
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                case 'u': {
                    if (r->pos + 4 > r->len) return 0;
                    u32 cp = 0;
                    for (int i = 0; i < 4; i++) { int h = hexval(r->b[r->pos + i]); if (h < 0) return 0; cp = (cp << 4) | (u32)h; }
                    r->pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        /* surrogate pair */
                        if (r->pos + 6 <= r->len && r->b[r->pos] == '\\' && r->b[r->pos + 1] == 'u') {
                            u32 lo = 0; int ok = 1;
                            for (int i = 0; i < 4; i++) { int h = hexval(r->b[r->pos + 2 + i]); if (h < 0) { ok = 0; break; } lo = (lo << 4) | (u32)h; }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                r->pos += 6;
                            } else cp = 0xFFFD;
                        } else cp = 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                    put_utf8(out, &o, cp);  /* output never exceeds input length */
                    break;
                }
                default: return 0;
            }
            continue;
        }
        out[o++] = c; r->pos++;
    }
    return 0;
}
static int jr_number(jr* r, i64* v) {
    u32 start = r->pos;
    if (r->pos < r->len && r->b[r->pos] == '-') r->pos++;
    while (r->pos < r->len && r->b[r->pos] >= '0' && r->b[r->pos] <= '9') r->pos++;
    u32 end = r->pos;
    /* skip fraction/exponent; we only keep the integer part */
    if (r->pos < r->len && r->b[r->pos] == '.') { r->pos++; while (r->pos < r->len && r->b[r->pos] >= '0' && r->b[r->pos] <= '9') r->pos++; }
    if (r->pos < r->len && (r->b[r->pos] == 'e' || r->b[r->pos] == 'E')) {
        r->pos++;
        if (r->pos < r->len && (r->b[r->pos] == '+' || r->b[r->pos] == '-')) r->pos++;
        while (r->pos < r->len && r->b[r->pos] >= '0' && r->b[r->pos] <= '9') r->pos++;
    }
    return k32_parse_i64(r->b + start, end - start, v);
}
static int jr_lit(jr* r, const char* lit) {
    u32 n = (u32)strlen(lit);
    if (r->pos + n <= r->len && memcmp(r->b + r->pos, lit, n) == 0) { r->pos += n; return 1; }
    return 0;
}
/* Skip a nested value (object/array) without recursion. */
static int jr_skip_nested(jr* r) {
    int depth = 0; int in_str = 0;
    while (r->pos < r->len) {
        char c = r->b[r->pos++];
        if (in_str) {
            if (c == '\\') { r->pos++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { depth--; if (depth == 0) return 1; }
    }
    return 0;
}
int k32_json_parse_flat(char* buf, u32 len, k32_jfield* out, u32 max_fields) {
    jr r = { buf, len, 0 };
    u32 n = 0;
    jr_ws(&r);
    if (r.pos >= r.len || r.b[r.pos] != '{') return -1;
    r.pos++;
    jr_ws(&r);
    if (r.pos < r.len && r.b[r.pos] == '}') { r.pos++; jr_ws(&r); return r.pos == r.len ? 0 : -1; }
    for (;;) {
        jr_ws(&r);
        char* k; u32 klen;
        if (!jr_string(&r, &k, &klen)) return -1;
        jr_ws(&r);
        if (r.pos >= r.len || r.b[r.pos] != ':') return -1;
        r.pos++;
        jr_ws(&r);
        k32_jfield tmp; k32_jfield* f = (n < max_fields) ? &out[n] : &tmp;
        memset(f, 0, sizeof *f);
        f->key = k; f->klen = klen;
        if (r.pos >= r.len) return -1;
        char c = r.b[r.pos];
        if (c == '"') {
            if (!jr_string(&r, &f->s, &f->slen)) return -1;
            f->type = K32_J_STR;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            if (!jr_number(&r, &f->i)) return -1;
            f->type = K32_J_INT;
        } else if (c == 't') { if (!jr_lit(&r, "true")) return -1; f->type = K32_J_BOOL; f->b = 1; }
        else if (c == 'f') { if (!jr_lit(&r, "false")) return -1; f->type = K32_J_BOOL; f->b = 0; }
        else if (c == 'n') { if (!jr_lit(&r, "null")) return -1; f->type = K32_J_NULL; }
        else if (c == '[') {
            r.pos++;
            f->type = K32_J_ARR;
            jr_ws(&r);
            if (r.pos < r.len && r.b[r.pos] == ']') { r.pos++; }
            else for (;;) {
                jr_ws(&r);
                if (r.pos >= r.len) return -1;
                k32_jelem etmp; k32_jelem* e = (f->arr_n < K32_JARR_MAX) ? &f->arr[f->arr_n] : &etmp;
                memset(e, 0, sizeof *e);
                char ec = r.b[r.pos];
                if (ec == '"') { if (!jr_string(&r, &e->s, &e->slen)) return -1; e->is_str = 1; }
                else if (ec == '-' || (ec >= '0' && ec <= '9')) { if (!jr_number(&r, &e->i)) return -1; e->is_int = 1; }
                else if (ec == 't') { if (!jr_lit(&r, "true")) return -1; e->i = 1; }
                else if (ec == 'f') { if (!jr_lit(&r, "false")) return -1; }
                else if (ec == 'n') { if (!jr_lit(&r, "null")) return -1; }
                else if (ec == '{' || ec == '[') { if (!jr_skip_nested(&r)) return -1; }
                else return -1;
                if (f->arr_n < K32_JARR_MAX) f->arr_n++;
                jr_ws(&r);
                if (r.pos >= r.len) return -1;
                if (r.b[r.pos] == ',') { r.pos++; continue; }
                if (r.b[r.pos] == ']') { r.pos++; break; }
                return -1;
            }
        } else if (c == '{') {
            if (!jr_skip_nested(&r)) return -1;
            f->type = K32_J_OTHER;
        } else return -1;
        if (n < max_fields) n++;
        jr_ws(&r);
        if (r.pos >= r.len) return -1;
        if (r.b[r.pos] == ',') { r.pos++; continue; }
        if (r.b[r.pos] == '}') { r.pos++; break; }
        return -1;
    }
    jr_ws(&r);
    if (r.pos != r.len) return -1;
    return (int)n;
}
const k32_jfield* k32_jf_get(const k32_jfield* f, int n, const char* key) {
    for (int i = 0; i < n; i++) if (k32_streq(f[i].key, f[i].klen, key)) return &f[i];
    return 0;
}
