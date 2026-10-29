/*
 * Applies tessera-db records (common/tlog.h) to the in-memory store. The
 * same code runs for the load at startup and for every change the API makes,
 * so the store always equals a replay of the log.
 */
#ifndef TESSERA_JOURNAL_H
#define TESSERA_JOURNAL_H
#include "k32lib.h"
#include "../../common/tlog.h"

enum {
    J_OK = 0,
    J_BAD = -1,        /* malformed payload or values out of range */
    J_ID = -2,         /* id is not the next one, or names a missing issue */
    J_FULL = -3        /* a table or the string arena is full */
};

/* Applies one framed record (TLOG_PAD and unknown types are skipped). */
int journal_apply(const u8* rec);
#endif
