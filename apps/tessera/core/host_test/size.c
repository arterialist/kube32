/* Prints the memory layout after k32_init with the current environment. */
#include "harness.h"
#include <stdlib.h>

int main(void) {
    shim.quiet = 1;
    int rc = k32_init();
    if (rc != 0) { fprintf(stderr, "k32_init failed: %d\n", rc); return 1; }
    int ticks = hx_seed_complete();
    tessera_layout L; tessera_layout_get(&L);
    const char* si = getenv("SEED_ISSUES"), *sc = getenv("SEED_COMMENTS");
    printf("tessera-core memory layout (SEED_ISSUES=%s SEED_COMMENTS=%s)\n", si ? si : "3000 (default)", sc ? sc : "6000 (default)");
    printf("  arena size            %9u bytes (%u KiB)\n", L.arena_size, L.arena_size / 1024);
    printf("  arena used            %9u bytes (%u KiB)  %u%%\n", L.arena_used, L.arena_used / 1024, (unsigned)((uint64_t)L.arena_used * 100 / L.arena_size));
    printf("  arena free            %9u bytes (%u KiB)\n", L.arena_size - L.arena_used, (L.arena_size - L.arena_used) / 1024);
    printf("  slab tables           %9u bytes\n", L.tables);
    printf("    issues     %5u x %2u B = %8u  (used %u)\n", L.issue_cap, L.issue_rec, L.issue_cap * L.issue_rec, L.issue_count);
    printf("    comments   %5u x %2u B = %8u  (used %u)\n", L.comment_cap, L.comment_rec, L.comment_cap * L.comment_rec, L.comment_count);
    printf("    terms      %5u x %2u B = %8u  (used %u)\n", L.term_cap, L.term_rec, L.term_cap * L.term_rec, L.term_count);
    printf("    activity     512 x %2u B = %8u\n", L.act_rec, 512 * L.act_rec);
    printf("    sessions      32 x %2u B = %8u\n", L.session_rec, 32 * L.session_rec);
    printf("  request scratch       %9u bytes\n", L.scratch);
    printf("  strings               %9u bytes\n", L.strings);
    printf("  postings              %9u bytes (%u postings, %u leaked by regrowth)\n", L.postings, L.posting_count, L.postings_leaked);
    printf("  seeding               %d ticks (SEED_CHUNK=%s), %u posting compactions\n", ticks, getenv("SEED_CHUNK") ? getenv("SEED_CHUNK") : "10 (default)", k32_prof.compactions);
    return 0;
}
