/* xfer-bench body format, shared by the server and the client.
 *
 * A body of n bytes (n a multiple of 4, 8 to XFER_MAX) holds n / 4 - 1
 * little-endian words w[i] = (i + 1) * 2654435761 ^ n, then a 4-byte
 * trailer: the FNV-1a of those words, one 32-bit word a step. The client
 * checks the size, the trailer against the words it received and the
 * trailer against the value the formula gives for n. */
#ifndef XFER_H
#define XFER_H

#include <stdint.h>

#define XFER_MAX 16384u

static inline uint32_t xfer_word(uint32_t n, uint32_t i) { return (i + 1u) * 2654435761u ^ n; }

static inline uint32_t xfer_step(uint32_t h, uint32_t w) { return (h ^ w) * 16777619u; }

#define XFER_SEED 2166136261u

#endif
