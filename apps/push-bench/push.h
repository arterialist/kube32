/* push-bench chunk format, shared by the publisher and the subscriber.
 *
 * A frame of `size` bytes goes out as `count` events on one topic, each a
 * 16-byte header and up to (chunk - 16) bytes of the frame:
 *
 *   0  'D'            magic
 *   1  run            run number, from 1
 *   2  idx            chunk index, 0 .. count - 1
 *   3  count          chunks in the frame (at most 192)
 *   4  frame          u32, from 0 in each run
 *   8  t_ms           u32, the publisher's wall clock (Unix ms, low 32 bits)
 *                     when the frame's first chunk went out
 *  12  size           u16, the frame's bytes
 *  14  fps            u8, the run's frame rate
 *  15  per            u8, frame bytes in every chunk but the last
 *
 * Chunk idx carries frame bytes [idx * per, min(size, (idx + 1) * per)).
 * Frame byte i is push_byte(frame, i), so every chunk can be checked on
 * arrival without holding the frame. */
#ifndef PUSH_H
#define PUSH_H

#include <stdint.h>

#define PUSH_HDR 16u
#define PUSH_MAX_CHUNKS 192u

static inline uint8_t push_byte(uint32_t frame, uint32_t i) { return (uint8_t)((i * 131u + frame * 7u) ^ (i >> 8)); }

static inline uint32_t push_rd32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void push_wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#endif
