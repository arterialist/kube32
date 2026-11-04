// SPDX-License-Identifier: GPL-2.0-or-later
/* The C library subset the DOOM engine uses, for a freestanding wasm32 pod.
 * Every standard header in this directory includes this one. Files open
 * blobs (k32_blob_*); output goes to the pod log (k32_log). */
#ifndef K32_LIBC_H
#define K32_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

/* string.h, strings.h */
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t n);
char* strcpy(char* d, const char* s);
char* strncpy(char* d, const char* s, size_t n);
char* strcat(char* d, const char* s);
char* strncat(char* d, const char* s, size_t n);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* h, const char* n);
char* strdup(const char* s);
char* strerror(int e);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);

/* ctype.h */
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* stdlib.h */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void* malloc(size_t n);
void* calloc(size_t n, size_t m);
void* realloc(void* p, size_t n);
void free(void* p);
int atoi(const char* s);
long atol(const char* s);
double atof(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
_Noreturn void exit(int code);
_Noreturn void abort(void);
char* getenv(const char* name);
int system(const char* cmd);
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
int atexit(void (*fn)(void));

/* stdio.h */
typedef struct k32_file FILE;
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 512
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int sprintf(char* s, const char* fmt, ...);
int snprintf(char* s, size_t n, const char* fmt, ...);
int vsprintf(char* s, const char* fmt, va_list ap);
int vsnprintf(char* s, size_t n, const char* fmt, va_list ap);
int sscanf(const char* s, const char* fmt, ...);
int puts(const char* s);
int putchar(int c);
int fputs(const char* s, FILE* f);
int fputc(int c, FILE* f);
#define putc fputc
int getc(FILE* f);
#define fgetc getc
char* fgets(char* s, int n, FILE* f);
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
size_t fread(void* p, size_t size, size_t n, FILE* f);
size_t fwrite(const void* p, size_t size, size_t n, FILE* f);
int fseek(FILE* f, long off, int whence);
long ftell(FILE* f);
int fflush(FILE* f);
int feof(FILE* f);
int remove(const char* path);
int rename(const char* a, const char* b);

/* math.h */
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double floor(double x) { return __builtin_floor(x); }
static inline double ceil(double x) { return __builtin_ceil(x); }
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);

/* assert.h */
_Noreturn void k32_libc_assert_fail(const char* expr, const char* file, int line);
#define assert(e) ((e) ? (void)0 : k32_libc_assert_fail(#e, __FILE__, __LINE__))

/* errno.h */
extern int errno;
#define ENOENT 2
#define EIO 5
#define ENOMEM 12
#define EEXIST 17
#define EISDIR 21
#define EINVAL 22

/* sys/types.h, unistd.h, fcntl.h, sys/stat.h */
typedef long ssize_t;
typedef long off_t;
typedef int mode_t;
int usleep(unsigned us);
int access(const char* path, int mode);
int unlink(const char* path);
#define F_OK 0
#define R_OK 4
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_BINARY 0
struct stat { off_t st_size; mode_t st_mode; };
int mkdir(const char* path, mode_t mode);
int stat(const char* path, struct stat* st);
#define S_ISDIR(m) 0

/* inttypes.h */
#define PRIi64 "lli"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"

#endif
