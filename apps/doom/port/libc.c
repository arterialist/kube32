// SPDX-License-Identifier: GPL-2.0-or-later
/* The C library subset the DOOM engine uses, for a freestanding wasm32 pod.
 *
 * malloc takes memory from the end of the module's data and grows the linear
 * memory a page at a time (memory.grow), so the pod's memory is what the
 * engine allocates. fopen opens one blob read-only: the IWAD named by the
 * env DOOM_IWAD is the blob named by DOOM_WAD_BLOB. stdout and stderr go to
 * the pod log a line at a time. */
#include "include/k32libc.h"
#include "../../../include/runtime/k32_app_abi.h"

int errno;

/* ---- memory ------------------------------------------------------------ */

extern unsigned char __heap_base;

typedef struct block {
    uint32_t size;          /* bytes after the header; low bit set while in use */
    uint32_t prev_size;     /* size of the block before, 0 for the first */
} block_t;

#define HDR ((uint32_t)sizeof(block_t))
#define ALIGN 8u
static uint8_t* heap_start;
static uint8_t* heap_end;   /* end of the last block */
uint32_t k32_libc_heap_peak;

static block_t* next_block(block_t* b) { return (block_t*)((uint8_t*)b + HDR + (b->size & ~1u)); }

static int grow_to(uint8_t* want) {
    uint32_t have = (uint32_t)__builtin_wasm_memory_size(0) * 65536u;
    if ((uint32_t)(uintptr_t)want <= have) return 1;
    uint32_t pages = ((uint32_t)(uintptr_t)want - have + 65535u) / 65536u;
    return __builtin_wasm_memory_grow(0, pages) != (size_t)-1;
}

void* malloc(size_t n) {
    if (!heap_start) {
        heap_start = (uint8_t*)(((uintptr_t)&__heap_base + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));
        heap_end = heap_start;
    }
    uint32_t need = (uint32_t)((n + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    if (need == 0) need = ALIGN;
    /* first fit */
    for (block_t* b = (block_t*)heap_start; (uint8_t*)b < heap_end; b = next_block(b)) {
        if (b->size & 1u) continue;
        if (b->size >= need) {
            if (b->size >= need + HDR + 16) {
                uint32_t rest = b->size - need - HDR;
                b->size = need;
                block_t* r = next_block(b);
                r->size = rest;
                r->prev_size = need;
                block_t* after = next_block(r);
                if ((uint8_t*)after < heap_end) after->prev_size = rest;
            }
            b->size |= 1u;
            return (uint8_t*)b + HDR;
        }
    }
    block_t* b = (block_t*)heap_end;
    if (!grow_to(heap_end + HDR + need)) return 0;
    b->size = need | 1u;
    b->prev_size = 0;
    if ((uint8_t*)b > heap_start) {
        /* find the previous block's size by walking is slow; keep it on the side */
    }
    heap_end += HDR + need;
    if ((uint32_t)(uintptr_t)heap_end > k32_libc_heap_peak) k32_libc_heap_peak = (uint32_t)(uintptr_t)heap_end;
    return (uint8_t*)b + HDR;
}

void free(void* p) {
    if (!p) return;
    block_t* b = (block_t*)((uint8_t*)p - HDR);
    b->size &= ~1u;
    /* merge with the free blocks after it */
    for (;;) {
        block_t* n = next_block(b);
        if ((uint8_t*)n >= heap_end || (n->size & 1u)) break;
        b->size += HDR + n->size;
    }
    block_t* n = next_block(b);
    if ((uint8_t*)n < heap_end) n->prev_size = b->size;
    else heap_end = (uint8_t*)b;   /* the last block returns to the top */
}

void* calloc(size_t n, size_t m) {
    void* p = malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    block_t* b = (block_t*)((uint8_t*)p - HDR);
    uint32_t have = b->size & ~1u;
    if (n <= have) return p;
    void* q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, have);
    free(p);
    return q;
}

/* ---- strings ----------------------------------------------------------- */

void* memcpy(void* d, const void* s, size_t n) { return __builtin_memcpy(d, s, n); }
void* memmove(void* d, const void* s, size_t n) { return __builtin_memmove(d, s, n); }
void* memset(void* d, int c, size_t n) { return __builtin_memset(d, c, n); }
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = s;
    for (; n; n--, p++) if (*p == (uint8_t)c) return (void*)p;
    return 0;
}
size_t strlen(const char* s) { const char* p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strncat(char* d, const char* s, size_t n) {
    char* p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}
int strcasecmp(const char* a, const char* b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((uint8_t)*a), y = tolower((uint8_t)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}
char* strrchr(const char* s, int c) {
    const char* r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char*)r;
    }
}
char* strstr(const char* h, const char* n) {
    size_t k = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, k)) return (char*)h;
    return k ? 0 : (char*)h;
}
char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
char* strerror(int e) { (void)e; return "error"; }

/* ---- numbers ----------------------------------------------------------- */

long strtol(const char* s, char** end, int base) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    long v = 0;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char*)s;
    return neg ? -v : v;
}
unsigned long strtoul(const char* s, char** end, int base) { return (unsigned long)strtol(s, end, base); }
int atoi(const char* s) { return (int)strtol(s, 0, 10); }
long atol(const char* s) { return strtol(s, 0, 10); }
double atof(const char* s) {
    char* e;
    double v = (double)strtol(s, &e, 10);
    if (*e == '.') {
        double f = 0.1;
        for (e++; isdigit(*e); e++, f /= 10) v += (s[0] == '-' ? -1 : 1) * (*e - '0') * f;
    }
    return v;
}

void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    /* insertion sort: the engine sorts short lists */
    uint8_t* a = base;
    uint8_t tmp[64];
    if (size > sizeof tmp) return;
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) { memcpy(a + j * size, a + (j - 1) * size, size); j--; }
        memcpy(a + j * size, tmp, size);
    }
}

double pow(double x, double y) {
    double r = 1;
    int n = (int)y;
    for (int i = 0; i < (n < 0 ? -n : n); i++) r *= x;
    return n < 0 ? 1 / r : r;
}
double sin(double x) { (void)x; return 0; }
double cos(double x) { (void)x; return 1; }
double tan(double x) { (void)x; return 0; }
double atan(double x) { (void)x; return 0; }
double atan2(double y, double x) { (void)y; (void)x; return 0; }

/* ---- process ----------------------------------------------------------- */

static char logline[512];
static uint32_t loglen;

static void log_flush(void) {
    if (loglen) k32_log(logline, loglen);
    loglen = 0;
}

static void log_char(char c) {
    if (c == '\n') { log_flush(); return; }
    if (loglen < sizeof logline) logline[loglen++] = c;
    if (loglen == sizeof logline) log_flush();
}

_Noreturn void exit(int code) {
    log_flush();
    char m[32];
    int n = snprintf(m, sizeof m, "exit(%d)", code);
    k32_log(m, (uint32_t)n);
    __builtin_trap();
}
_Noreturn void abort(void) { exit(134); }
_Noreturn void k32_libc_assert_fail(const char* expr, const char* file, int line) {
    printf("assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}
char* getenv(const char* name) { (void)name; return 0; }
int system(const char* cmd) { (void)cmd; return -1; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }
int usleep(unsigned us) { (void)us; return 0; }
int access(const char* path, int mode) { (void)mode; FILE* f = fopen(path, "rb"); if (!f) return -1; fclose(f); return 0; }
int unlink(const char* path) { (void)path; return -1; }
int remove(const char* path) { (void)path; return -1; }
int rename(const char* a, const char* b) { (void)a; (void)b; return -1; }
int mkdir(const char* path, mode_t mode) { (void)path; (void)mode; return -1; }
int stat(const char* path, struct stat* st) { (void)path; (void)st; return -1; }

/* ---- files: blobs ------------------------------------------------------ */

struct k32_file {
    int32_t blob;      /* -1 for the log streams */
    uint32_t pos, size;
};
static struct k32_file std_files[3] = { { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 } };
FILE* stdin = &std_files[0];
FILE* stdout = &std_files[1];
FILE* stderr = &std_files[2];
static struct k32_file wad_file;
uint32_t k32_libc_blob_reads, k32_libc_blob_bytes;

static int env(const char* key, char* out, uint32_t cap) {
    int32_t n = k32_env_get(key, (uint32_t)strlen(key), out, cap - 1);
    if (n < 0 || (uint32_t)n >= cap) return 0;
    out[n] = 0;
    return 1;
}

FILE* fopen(const char* path, const char* mode) {
    char iwad[48], blob[48];
    if (mode[0] != 'r' || strchr(mode, '+')) return 0;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!env("DOOM_IWAD", iwad, sizeof iwad)) strcpy(iwad, "freedoom1.wad");
    if (!env("DOOM_WAD_BLOB", blob, sizeof blob)) strcpy(blob, "doom-wad");
    if (strcasecmp(base, iwad) || wad_file.size) return 0;
    int32_t h = k32_blob_open(blob, (uint32_t)strlen(blob));
    if (h < 0) {
        printf("blob %s: error %d\n", blob, (int)h);
        return 0;
    }
    wad_file.blob = h;
    wad_file.pos = 0;
    wad_file.size = (uint32_t)k32_blob_size(h);
    return &wad_file;
}

int fclose(FILE* f) {
    if (f && f->blob >= 0) { k32_blob_close(f->blob); f->blob = -1; f->size = 0; }
    return 0;
}

size_t fread(void* p, size_t size, size_t n, FILE* f) {
    if (!f || f->blob < 0 || size == 0) return 0;
    uint32_t want = (uint32_t)(size * n);
    if (f->pos >= f->size) return 0;
    if (want > f->size - f->pos) want = f->size - f->pos;
    int32_t got = k32_blob_read(f->blob, f->pos, p, want);
    if (got <= 0) return 0;
    k32_libc_blob_reads++;
    k32_libc_blob_bytes += (uint32_t)got;
    f->pos += (uint32_t)got;
    return (size_t)got / size;
}

int fseek(FILE* f, long off, int whence) {
    if (!f) return -1;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->pos : (long)f->size;
    f->pos = (uint32_t)(base + off);
    return 0;
}
long ftell(FILE* f) { return f ? (long)f->pos : -1; }
int feof(FILE* f) { return f && f->pos >= f->size; }
int fflush(FILE* f) { (void)f; log_flush(); return 0; }
int getc(FILE* f) { uint8_t c; return fread(&c, 1, 1, f) == 1 ? c : EOF; }
char* fgets(char* s, int n, FILE* f) {
    int i = 0;
    while (i < n - 1) { int c = getc(f); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    s[i] = 0;
    return i ? s : 0;
}
size_t fwrite(const void* p, size_t size, size_t n, FILE* f) {
    if (f == stdout || f == stderr) { const char* c = p; for (size_t i = 0; i < size * n; i++) log_char(c[i]); return n; }
    return 0;
}
int fputc(int c, FILE* f) { char ch = (char)c; fwrite(&ch, 1, 1, f); return c; }
int fputs(const char* s, FILE* f) { fwrite(s, 1, strlen(s), f); return 0; }
int putchar(int c) { log_char((char)c); return c; }
int puts(const char* s) { while (*s) log_char(*s++); log_char('\n'); return 0; }

/* ---- formatted output -------------------------------------------------- */

typedef struct { char* buf; size_t cap, len; } out_t;
static void out_c(out_t* o, char c) { if (o->len + 1 < o->cap) o->buf[o->len] = c; o->len++; }

static void out_num(out_t* o, unsigned long long v, int base, int upper, int neg, int width, int zero, int left, int prec, char sign) {
    char d[24];
    int n = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { d[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    while (n < prec) d[n++] = '0';
    char s = neg ? '-' : sign;
    int len = n + (s ? 1 : 0);
    if (!left && !zero) for (; width > len; width--) out_c(o, ' ');
    if (s) out_c(o, s);
    if (!left && zero) for (; width > len; width--) out_c(o, '0');
    while (n) out_c(o, d[--n]);
    if (left) for (; width > len; width--) out_c(o, ' ');
}

int vsnprintf(char* buf, size_t cap, const char* f, va_list ap) {
    out_t o = { buf, cap, 0 };
    for (; *f; f++) {
        if (*f != '%') { out_c(&o, *f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        char sign = 0;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') sign = '+';
            else if (*f == ' ') sign = ' ';
            else if (*f == '#') {}
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (isdigit(*f)) prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'h' || *f == 'z') { if (*f == 'l') lng++; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            out_num(&o, v < 0 ? (unsigned long long)-v : (unsigned long long)v, 10, 0, v < 0, width, zero, left, prec, sign);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            out_num(&o, v, *f == 'u' ? 10 : *f == 'o' ? 8 : 16, *f == 'X', 0, width, zero, left, prec, 0);
            break;
        }
        case 'p': out_c(&o, '0'); out_c(&o, 'x'); out_num(&o, (uintptr_t)va_arg(ap, void*), 16, 0, 0, 0, 0, 0, -1, 0); break;
        case 'c': out_c(&o, (char)va_arg(ap, int)); break;
        case 'f': case 'g': case 'e': {
            double v = va_arg(ap, double);
            int neg = v < 0;
            if (neg) v = -v;
            if (prec < 0) prec = 6;
            unsigned long long ip = (unsigned long long)v;
            out_num(&o, ip, 10, 0, neg, 0, 0, 0, -1, sign);
            if (prec) {
                out_c(&o, '.');
                double fr = v - (double)ip;
                for (int i = 0; i < prec; i++) { fr *= 10; int dg = (int)fr; out_c(&o, (char)('0' + dg)); fr -= dg; }
            }
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int n = (int)strlen(s);
            if (prec >= 0 && n > prec) n = prec;
            if (!left) for (int w = width; w > n; w--) out_c(&o, ' ');
            for (int i = 0; i < n; i++) out_c(&o, s[i]);
            if (left) for (int w = width; w > n; w--) out_c(&o, ' ');
            break;
        }
        case '%': out_c(&o, '%'); break;
        default: out_c(&o, '%'); out_c(&o, *f); break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int vsprintf(char* s, const char* f, va_list ap) { return vsnprintf(s, 1 << 20, f, ap); }
int snprintf(char* s, size_t n, const char* f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(s, n, f, ap); va_end(ap); return r; }
int sprintf(char* s, const char* f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(s, 1 << 20, f, ap); va_end(ap); return r; }

static char pbuf[512];
int vfprintf(FILE* fp, const char* f, va_list ap) {
    int n = vsnprintf(pbuf, sizeof pbuf, f, ap);
    fwrite(pbuf, 1, (size_t)(n < (int)sizeof pbuf ? n : (int)sizeof pbuf - 1), fp);
    return n;
}
int vprintf(const char* f, va_list ap) { return vfprintf(stdout, f, ap); }
int printf(const char* f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(stdout, f, ap); va_end(ap); return r; }
int fprintf(FILE* fp, const char* f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(fp, f, ap); va_end(ap); return r; }

/* sscanf: %d %i %u %x %s %c and literal characters, what config parsing needs */
int sscanf(const char* s, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    int got = 0;
    for (; *f; f++) {
        if (isspace(*f)) { while (isspace(*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) break; s++; continue; }
        f++;
        while (*f == 'l' || *f == 'h') f++;
        if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x') {
            char* e;
            long v = strtol(s, &e, *f == 'x' ? 16 : *f == 'i' ? 0 : 10);
            if (e == s) break;
            *va_arg(ap, int*) = (int)v;
            s = e;
            got++;
        } else if (*f == 's') {
            while (isspace(*s)) s++;
            char* d = va_arg(ap, char*);
            if (!*s) break;
            while (*s && !isspace(*s)) *d++ = *s++;
            *d = 0;
            got++;
        } else if (*f == 'c') {
            if (!*s) break;
            *va_arg(ap, char*) = *s++;
            got++;
        } else break;
    }
    va_end(ap);
    return got;
}
