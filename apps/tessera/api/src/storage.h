/*
 * tessera-api's link to tessera-db (Service "tessera-db", see
 * ../../db/src/main.c). At startup the store is empty: k32_tick loads the
 * log page by page while requests are answered from what is loaded. Every
 * change is first appended to the log and applied to the store only once
 * tessera-db has acknowledged it, so a 2xx answer means the change is on
 * the card.
 */
#ifndef TESSERA_STORAGE_H
#define TESSERA_STORAGE_H
#include "k32lib.h"

/* Reads TESSERA_DB_URL and LOAD_CHUNK; the load starts on the next tick. */
void storage_init(void);
/* One step of the load: 1 while more work is ready, 0 when idle or waiting. */
int  storage_tick(void);
/* 1 when changes are accepted (the load finished and the log is healthy). */
int  storage_writable(void);
/* Appends one encoded record (common/tlog.h, seq 0) to tessera-db and, once
 * acknowledged, applies it to the store. Returns 0, or the HTTP status to
 * answer with: 503 (loading, tessera-db unreachable or read-only, log
 * changed), 507 (full). */
int  storage_commit(u8* rec, u32 len);
/* Message for a refused change ("loading data", "tessera-db unreachable"). */
const char* storage_unavailable_reason(void);
/* {"state":...,"seq":...} for /api/stats, and the "loading" progress. */
void storage_write_stats(k32_jw* w);
#endif
