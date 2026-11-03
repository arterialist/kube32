/* tokenizer.h: llama2.c's tokenizer for the pods of llm-pipeline and
 * llm-speculative: tokenizer.bin parsed in place, llama2.c's encode (BPE
 * merges by score) and the text of a token. */
#ifndef LLM_TOKENIZER_H
#define LLM_TOKENIZER_H

#include "llm.h"

#define VOCAB_MAX 1024
#define HASH_SIZE 2048

static const uint8_t* g_tok_raw;
static uint32_t g_vocab, g_max_len;
static uint32_t g_off[VOCAB_MAX];
static uint8_t g_len[VOCAB_MAX];
static float g_score[VOCAB_MAX];
static uint16_t g_hash[HASH_SIZE];

static uint32_t tok_hash(const uint8_t* s, uint32_t n) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) { h ^= s[i]; h *= 0x01000193u; }
    return h;
}

static int32_t tok_lookup(const uint8_t* s, uint32_t n) {
    for (uint32_t h = tok_hash(s, n) & (HASH_SIZE - 1);; h = (h + 1) & (HASH_SIZE - 1)) {
        const uint16_t id = g_hash[h];
        if (id == 0xffff) return -1;
        if (g_len[id] == n) {
            const uint8_t* p = g_tok_raw + g_off[id];
            uint32_t i = 0;
            while (i < n && p[i] == s[i]) i++;
            if (i == n) return id;
        }
    }
}

/* tokenizer.bin as llama2.c writes it, n bytes at raw (kept): int max_len,
 * then per token float score, int len, len bytes. Returns the vocabulary
 * size, 0 for a file that is not one. */
static uint32_t tok_parse(const uint8_t* raw, uint32_t n) {
    uint32_t p = 4, v = 0;
    g_tok_raw = raw;
    __builtin_memcpy(&g_max_len, raw, 4);
    for (uint32_t i = 0; i < HASH_SIZE; i++) g_hash[i] = 0xffff;
    while (p + 8 <= n && v < VOCAB_MAX) {
        uint32_t len;
        __builtin_memcpy(&g_score[v], raw + p, 4);
        __builtin_memcpy(&len, raw + p + 4, 4);
        p += 8;
        if (len > 255 || p + len > n) break;
        g_off[v] = p;
        g_len[v] = (uint8_t)len;
        uint32_t h = tok_hash(raw + p, len) & (HASH_SIZE - 1);
        while (g_hash[h] != 0xffff) h = (h + 1) & (HASH_SIZE - 1);
        g_hash[h] = (uint16_t)v;
        p += len;
        v++;
    }
    g_vocab = p == n && v >= 3 ? v : 0;
    return g_vocab;
}

/* llama2.c's encode: BOS, a space in front, one token per UTF-8 character
 * (bytes + 3 where there is none), then the best-scored merges */
static uint32_t tok_encode(const char* text, uint32_t n, uint16_t* toks, uint32_t cap) {
    uint32_t nt = 0;
    toks[nt++] = (uint16_t)LLM_BOS;
    if (n) {
        int32_t sp = tok_lookup((const uint8_t*)" ", 1);
        if (sp >= 0) toks[nt++] = (uint16_t)sp;
    }
    for (uint32_t i = 0; i < n && nt < cap;) {
        uint32_t cl = 1;
        while (i + cl < n && cl < 4 && ((uint8_t)text[i + cl] & 0xc0) == 0x80) cl++;
        int32_t id = tok_lookup((const uint8_t*)text + i, cl);
        if (id >= 0) toks[nt++] = (uint16_t)id;
        else for (uint32_t b = 0; b < cl && nt < cap; b++) toks[nt++] = (uint16_t)((uint8_t)text[i + b] + 3);
        i += cl;
    }
    uint8_t pair[512];
    for (;;) {
        float best = -1e10f;
        int32_t best_id = -1, best_at = -1;
        for (uint32_t i = 0; i + 1 < nt; i++) {
            const uint32_t a = g_len[toks[i]], b = g_len[toks[i + 1]];
            if (a + b > sizeof pair) continue;
            __builtin_memcpy(pair, g_tok_raw + g_off[toks[i]], a);
            __builtin_memcpy(pair + a, g_tok_raw + g_off[toks[i + 1]], b);
            const int32_t id = tok_lookup(pair, a + b);
            if (id >= 0 && g_score[id] > best) { best = g_score[id]; best_id = id; best_at = (int32_t)i; }
        }
        if (best_at < 0) break;
        toks[best_at] = (uint16_t)best_id;
        for (uint32_t i = (uint32_t)best_at + 1; i + 1 < nt; i++) toks[i] = toks[i + 1];
        nt--;
    }
    return nt;
}

/* The text of token `tok` after `prev`: no space right after BOS, <0xHH>
 * as its byte, unprintable bytes left out */
static uint32_t tok_piece(uint32_t prev, uint32_t tok, char* out) {
    const uint8_t* p = g_tok_raw + g_off[tok];
    uint32_t n = g_len[tok];
    if (prev == LLM_BOS && n && p[0] == ' ') { p++; n--; }
    if (n == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' && p[5] == '>') {
        uint32_t b = 0;
        for (int i = 3; i < 5; i++) b = b * 16 + (uint32_t)(p[i] <= '9' ? p[i] - '0' : (p[i] | 0x20) - 'a' + 10);
        if (b < 0x20 && b != '\n') return 0;
        out[0] = (char)b;
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) out[i] = (char)p[i];
    return n;
}

#endif
