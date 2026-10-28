/*
 * k32lib: tiny freestanding runtime for Kube32 Wasm workloads.
 * No libc. Everything is bounds-checked; nothing recurses.
 */
#ifndef K32LIB_H
#define K32LIB_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint64_t u64;

/* ---- work counters (native builds only; compiled out of the wasm) ------- */
#if defined(K32_NATIVE)
typedef struct {
    u32 cmp_calls;      /* issue comparator calls (sorting / top-k) */
    u32 term_cmps;      /* string compares in term dictionary lookups */
    u32 stop_cmps;      /* string compares in stopword lookups */
    u32 posting_steps;  /* postings visited by find/remove scans */
    u32 compactions;    /* posting region compactions */
    u32 jw_putc;        /* single-byte writes (separators, quotes, braces) */
    u32 jw_writes;      /* bulk writes (memcpy) */
    u32 jw_write_bytes; /* bytes written in bulk */
    u32 jw_scan_bytes;  /* bytes that went through the escape scanner */
    u32 jw_keys, jw_ints;
    u32 resp_writes;    /* k32_resp_write calls (flushes) */
    u32 loop_iters;     /* loop iterations on the response path (memcpy words, scans, digits, labels, candidate scan) */
    u32 scan_issues;    /* issues visited by the candidate scan */
} k32_prof_t;
extern k32_prof_t k32_prof;
#define K32_PROF(field) ((void)(k32_prof.field++))
#else
#define K32_PROF(field) ((void)0)
#endif

/* ---- memory primitives (exported as plain symbols for clang) ---------- */
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);

/* ---- strings ------------------------------------------------------------ */
int  k32_streq(const char* a, u32 alen, const char* lit);           /* lit is NUL-terminated */
int  k32_strneq(const char* a, u32 alen, const char* b, u32 blen);
int  k32_strcmp(const char* a, u32 alen, const char* b, u32 blen);  /* lexicographic, <0 / 0 / >0 */
int  k32_starts_with(const char* s, u32 len, const char* prefix, u32 plen);
u32  k32_fmt_u32(char* out, u32 cap, u32 v);
u32  k32_fmt_i64(char* out, u32 cap, i64 v);
u32  k32_fmt_hex32(char* out, u32 cap, u32 v);                      /* 8 lowercase hex digits */
int  k32_parse_i64(const char* s, u32 len, i64* out);               /* 1 on success */
int  k32_parse_u32(const char* s, u32 len, u32* out);
u32  k32_url_decode(const char* in, u32 len, char* out, u32 cap);   /* %XX and '+' */
int  k32_query_get(const char* q, u32 qlen, const char* key, const char** val, u32* vlen);
u32  k32_utf8_backoff(const char* s, u32 len);                      /* trims a partial UTF-8 tail */

/* ---- tokenizer: lowercase alphanumeric runs ----------------------------- */
#define K32_TOKEN_MAX 24
typedef struct { const char* s; u32 len; u32 pos; } k32_tok;
void k32_tok_init(k32_tok* t, const char* s, u32 len);
u32  k32_tok_next(k32_tok* t, char* out, u32 cap);   /* returns token length, 0 when exhausted */
int  k32_is_stopword(const char* s, u32 len);               /* hashed lookup */
u32  k32_hash(const char* s, u32 len);                      /* FNV-1a */

/* ---- PRNG --------------------------------------------------------------- */
typedef struct { u32 s; } k32_rng;
void k32_rng_seed(k32_rng* r, u32 seed);
u32  k32_rng_next(k32_rng* r);
u32  k32_rng_range(k32_rng* r, u32 n);              /* [0, n) ; n == 0 returns 0 */

/* ---- two-ended bump arena -----------------------------------------------
 * [0, top) grows upward (tables, strings); [limit, size) grows downward
 * (posting blocks, see index.c). The two never cross. */
typedef struct { u8* base; u32 size; u32 top; u32 limit; } k32_arena;
void  k32_arena_init(k32_arena* a, u8* base, u32 size);
void* k32_arena_alloc(k32_arena* a, u32 size, u32 align);      /* NULL when full; zeroed */
void* k32_arena_alloc_top(k32_arena* a, u32 size, u32 align);  /* NULL when full; NOT zeroed */
u32   k32_arena_free(const k32_arena* a);                      /* bytes between the two ends */
u32   k32_arena_used(const k32_arena* a);                      /* top + top-region bytes */

/* ---- sorting (no recursion) --------------------------------------------- */
typedef int (*k32_cmp_u16)(u16 a, u16 b, void* ctx);
void k32_heapsort_u16(u16* a, u32 n, k32_cmp_u16 cmp, void* ctx);
void k32_sort_u32(u32* a, u32 n);
/* bounded top-k selection: see k32_topk_push */
typedef struct { u16* a; u32 n, k; k32_cmp_u16 cmp; void* ctx; } k32_topk;
void k32_topk_init(k32_topk* t, u16* buf, u32 k, k32_cmp_u16 cmp, void* ctx);
void k32_topk_push(k32_topk* t, u16 v);

/* ---- streaming JSON writer ---------------------------------------------- */
typedef int (*k32_jw_flush_fn)(void* ctx, const char* data, u32 len);
typedef struct {
    char* buf; u32 cap; u32 len;
    k32_jw_flush_fn flush; void* ctx;
    u8 first[16]; u8 depth; u8 after_key; u8 truncated;
    u32 total;
} k32_jw;
void k32_jw_init(k32_jw* w, char* buf, u32 cap, k32_jw_flush_fn flush, void* ctx);
void k32_jw_flush(k32_jw* w);
void k32_jw_raw(k32_jw* w, const char* s, u32 len);
void k32_jw_lit(k32_jw* w, const char* s);         /* raw NUL-terminated, no separator */
void k32_jw_obj_begin(k32_jw* w);
void k32_jw_obj_end(k32_jw* w);
void k32_jw_arr_begin(k32_jw* w);
void k32_jw_arr_end(k32_jw* w);
void k32_jw_key(k32_jw* w, const char* key);        /* dynamic key (escaped) */
void k32_jw_keyn(k32_jw* w, const char* frag, u32 n);   /* complete "\"key\":" fragment */
#define k32_jw_keylit(w, lit) k32_jw_keyn((w), "\"" lit "\":", (u32)sizeof(lit) + 2u)   /* literal key, one memcpy */
void k32_jw_str(k32_jw* w, const char* s, u32 len);
void k32_jw_str_raw(k32_jw* w, const char* s, u32 len);  /* no escaping: caller checked k32_jw_needs_escape */
int  k32_jw_needs_escape(const char* s, u32 len);
void k32_jw_str_max(k32_jw* w, const char* s, u32 len, u32 max_escaped);
void k32_jw_cstr(k32_jw* w, const char* s);
void k32_jw_int(k32_jw* w, i64 v);
void k32_jw_bool(k32_jw* w, int v);
void k32_jw_null(k32_jw* w);
void k32_jw_escaped(k32_jw* w, const char* s, u32 len);  /* escaped body without quotes/separator */

/* ---- flat JSON reader --------------------------------------------------- */
enum { K32_J_NULL = 0, K32_J_BOOL, K32_J_INT, K32_J_STR, K32_J_ARR, K32_J_OTHER };
#define K32_JARR_MAX 16
typedef struct { char* s; u32 slen; i64 i; u8 is_str; u8 is_int; } k32_jelem;
typedef struct {
    const char* key; u32 klen;
    u8 type; u8 b; u8 arr_n;
    i64 i;
    char* s; u32 slen;
    k32_jelem arr[K32_JARR_MAX];
} k32_jfield;
int k32_json_parse_flat(char* buf, u32 len, k32_jfield* out, u32 max_fields);  /* count or -1 */
const k32_jfield* k32_jf_get(const k32_jfield* f, int n, const char* key);

#endif
