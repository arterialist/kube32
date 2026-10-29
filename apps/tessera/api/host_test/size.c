/* Generates the dataset (DATASET_ISSUES / DATASET_COMMENTS, default 2400 /
 * 4800), loads it through the in-process tessera-db and prints the memory
 * layout, the dataset size and the load cost. */
#include "harness.h"
#include "../../db/src/tdb.h"

static uint32_t env_u32(const char* k, uint32_t d) { const char* v = getenv(k); return v ? (uint32_t)strtoul(v, 0, 10) : d; }

int main(void) {
    shim.quiet = 1;
    uint32_t ni = env_u32("DATASET_ISSUES", 2400), nc = env_u32("DATASET_COMMENTS", 4800);
    dataset_stats ds;
    double gen_ms = hx_db_setup(ni, nc, &ds);
    if (gen_ms < 0) { fprintf(stderr, "dataset failed: %f\n", gen_ms); return 1; }
    int rc = k32_init();
    if (rc != 0) { fprintf(stderr, "k32_init failed: %d\n", rc); return 1; }
    shim_fs.read_bytes = 0; shim_fs.reads = 0; shim.call_us = 0;
    double t0 = hx_now_ms(), max_tick = 0;
    int ticks = 0;
    for (;;) {
        double a = hx_now_ms();
        int more = k32_tick(0);
        double d = hx_now_ms() - a;
        if (d > max_tick) max_tick = d;
        ticks++;
        if (more <= 0) break;
    }
    double load_ms = hx_now_ms() - t0;
    shim_service* db = shim_service_get("tessera-db");
    tessera_layout L; tessera_layout_get(&L);
    printf("tessera-api memory layout (dataset %u issues, %u comments; MAX_ISSUES=%s MAX_COMMENTS=%s)\n", ni, nc,
           getenv("MAX_ISSUES") ? getenv("MAX_ISSUES") : "2900", getenv("MAX_COMMENTS") ? getenv("MAX_COMMENTS") : "5300");
    printf("  arena size            %9u bytes (%u KiB)\n", L.arena_size, L.arena_size / 1024);
    printf("  arena used            %9u bytes (%u KiB)  %u%%\n", L.arena_used, L.arena_used / 1024, (unsigned)((uint64_t)L.arena_used * 100 / L.arena_size));
    printf("  arena free            %9u bytes (%u KiB)\n", L.arena_size - L.arena_used, (L.arena_size - L.arena_used) / 1024);
    printf("  slab tables           %9u bytes (incl. the %u-byte record page)\n", L.tables, 16384u);
    printf("    issues     %5u x %2u B = %8u  (used %u)\n", L.issue_cap, L.issue_rec, L.issue_cap * L.issue_rec, L.issue_count);
    printf("    comments   %5u x %2u B = %8u  (used %u)\n", L.comment_cap, L.comment_rec, L.comment_cap * L.comment_rec, L.comment_count);
    printf("    terms      %5u x %2u B = %8u  (used %u)\n", L.term_cap, L.term_rec, L.term_cap * L.term_rec, L.term_count);
    printf("    activity     512 x %2u B = %8u\n", L.act_rec, 512 * L.act_rec);
    printf("    sessions      32 x %2u B = %8u\n", L.session_rec, 32 * L.session_rec);
    printf("  request scratch       %9u bytes\n", L.scratch);
    printf("  strings               %9u bytes\n", L.strings);
    printf("  postings              %9u bytes (%u postings, %u leaked by regrowth)\n", L.postings, L.posting_count, L.postings_leaked);
    printf("dataset (tessera-db format)\n");
    printf("  records               %9u (%u issues, %u status changes, %u comments)\n", ds.records, ds.issues, ds.updates, ds.comments);
    printf("  size                  %9u bytes in %u segments of <= %u bytes\n", ds.bytes, ds.segments, TDB_SEG_BYTES);
    printf("  generated in          %9.1f ms\n", gen_ms);
    printf("load through the native tessera-db shim\n");
    printf("  wall time             %9.1f ms (%d ticks, longest %.2f ms; tessera-db side %.1f ms)\n", load_ms, ticks, max_tick, shim.call_us / 1000.0);
    printf("  service calls         %9u (stats + record pages)\n", db ? db->calls : 0);
    printf("  card reads            %9llu bytes in %u reads (%.2fx the log)\n", (unsigned long long)shim_fs.read_bytes, shim_fs.reads, ds.bytes ? (double)shim_fs.read_bytes / ds.bytes : 0);
    hx_db_cleanup();
    return 0;
}
