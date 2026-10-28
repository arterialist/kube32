/*
 * Native assertions for tessera-core. Run with SEED_ISSUES=500 (see Makefile).
 */
#include "harness.h"
#include "../src/index.h"
#include "../src/store.h"
#include <stdlib.h>

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char cookie[64];
static char last_key[32];

/* every request must call resp_begin exactly once and return the status it sent */
static int req(const char* method, const char* path, const char* body) {
    int st = hx_request(method, path, cookie[0] ? cookie : 0, body);
    CHECK(shim.begin_calls == 1, "%s %s: resp_begin called %d times", method, path, shim.begin_calls);
    CHECK(shim.end_calls == 1, "%s %s: resp_end called %d times", method, path, shim.end_calls);
    CHECK(st == shim.status, "%s %s: returned %d but sent %d", method, path, st, shim.status);
    return st;
}
static int req_nocookie(const char* method, const char* path, const char* body) {
    int st = hx_request(method, path, 0, body);
    CHECK(shim.begin_calls == 1, "%s %s: resp_begin called %d times", method, path, shim.begin_calls);
    return st;
}
static long jint(const char* body, const char* key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(body, pat);
    if (!p) return -999999;
    return strtol(p + strlen(pat), 0, 10);
}
static int jstr(const char* body, const char* key, char* out, size_t cap) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char* p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 1;
}
static int count_items(const char* body) {
    const char* p = strstr(body, "\"items\":[");
    if (!p) return -1;
    p += 9;
    int depth = 0, n = 0;
    for (; *p; p++) {
        if (*p == '{') { if (depth == 0) n++; depth++; }
        else if (*p == '}') depth--;
        else if (*p == ']' && depth == 0) break;
    }
    return n;
}
static int count_substr(const char* body, const char* s) {
    int n = 0; size_t l = strlen(s);
    for (const char* p = body; (p = strstr(p, s)); p += l) n++;
    return n;
}
static int array_len(const char* body, const char* key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char* p = strstr(body, pat);
    if (!p) return -1;
    p += strlen(pat);
    if (*p == ']') return 0;
    int n = 1;
    for (; *p && *p != ']'; p++) if (*p == ',') n++;
    return n;
}
static const shim_event* find_event(const char* needle) {
    for (int i = 0; i < shim.event_n; i++) if (strstr(shim.events[i].data, needle)) return &shim.events[i];
    return 0;
}

static void test_auth(void) {
    cookie[0] = 0;
    CHECK(req_nocookie("GET", "/api/issues", 0) == 401, "list without session must be 401");
    CHECK(strstr(shim.body, "\"error\":\"unauthorized\"") != 0, "401 error shape");
    CHECK(req_nocookie("GET", "/api/me", 0) == 401, "me without session");
    CHECK(req_nocookie("POST", "/api/auth/login", "{\"username\":\"mara\",\"password\":\"nope\"}") == 401, "bad password");
    CHECK(req_nocookie("POST", "/api/auth/login", "{\"username\":\"keypad\",\"password\":\"demo\"}") == 401, "system user cannot log in");
    CHECK(req_nocookie("POST", "/api/auth/login", "{\"username\":\"mara\"") == 400, "malformed json login");
    CHECK(req_nocookie("POST", "/api/auth/login", "{\"username\":\"mara\",\"password\":\"demo\"}") == 200, "login ok");
    CHECK(strstr(shim.body, "\"username\":\"mara\"") != 0, "login returns user");
    CHECK(strstr(shim.body, "\"now\":") != 0, "login returns now");
    CHECK(shim.header_n == 1 && strcmp(shim.headers[0].name, "Set-Cookie") == 0, "Set-Cookie header");
    CHECK(strncmp(shim.headers[0].value, "tsid=", 5) == 0 && strstr(shim.headers[0].value, "HttpOnly") && strstr(shim.headers[0].value, "Path=/"), "cookie attrs: %s", shim.headers[0].value);
    snprintf(cookie, sizeof cookie, "other=1; tsid=%.16s; x=y", shim.headers[0].value + 5);
    CHECK(req("GET", "/api/me", 0) == 200, "me with session");
    CHECK(strstr(shim.body, "\"id\":1") != 0, "me returns mara id 1");
    /* logout drops the session */
    char saved[64]; strcpy(saved, cookie);
    CHECK(req("POST", "/api/auth/logout", 0) == 204, "logout 204");
    CHECK(req("GET", "/api/me", 0) == 401, "me after logout");
    /* log back in as dimitri for the rest */
    cookie[0] = 0;
    req_nocookie("POST", "/api/auth/login", "{\"username\":\"dimitri\",\"password\":\"demo\"}");
    snprintf(cookie, sizeof cookie, "tsid=%.16s", shim.headers[0].value + 5);
    CHECK(req("GET", "/api/me", 0) == 200 && strstr(shim.body, "\"id\":2"), "logged in as dimitri");
}

static long total_before_create;
static void test_bootstrap(void) {
    CHECK(req_nocookie("GET", "/api/bootstrap", 0) == 200, "bootstrap is public");
    CHECK(strstr(shim.body, "\"users\":[") && strstr(shim.body, "\"projects\":[") && strstr(shim.body, "\"labels\":[") &&
          strstr(shim.body, "\"statuses\":[\"backlog\",\"todo\",\"in_progress\",\"review\",\"done\"]") &&
          strstr(shim.body, "\"priorities\":[\"none\",\"low\",\"medium\",\"high\",\"urgent\"]") &&
          strstr(shim.body, "\"counts\":{\"open\":"), "bootstrap shape");
    CHECK(count_substr(shim.body, "\"username\":") == 9, "9 users (8 + keypad)");
    CHECK(count_substr(shim.body, "\"key\":\"") == 3, "3 projects");
    CHECK(array_len(shim.body, "statuses") == 5, "statuses");
    CHECK(jint(shim.body, "total") == 500, "total 500, got %ld", jint(shim.body, "total"));
    long open = jint(shim.body, "open"), done = jint(shim.body, "done");
    CHECK(open + done == 500, "open+done = total (%ld + %ld)", open, done);
    CHECK(done > 150 && done < 250, "~40%% done: %ld", done);
    total_before_create = jint(shim.body, "total");
}

static void test_list(void) {
    CHECK(req("GET", "/api/issues", 0) == 200, "list");
    CHECK(jint(shim.body, "total") == 500 && jint(shim.body, "page") == 1 && jint(shim.body, "limit") == 25, "defaults");
    CHECK(count_items(shim.body) == 25, "25 items, got %d", count_items(shim.body));
    CHECK(strstr(shim.body, "\"description\"") == 0, "summary omits description");
    CHECK(strstr(shim.body, "\"took_us\":") && strstr(shim.body, "\"now\":"), "took_us/now");
    jstr(shim.body, "key", last_key, sizeof last_key);
    /* updated desc ordering */
    {
        const char* p = strstr(shim.body, "\"updated\":"); long a = strtol(p + 10, 0, 10);
        p = strstr(p + 10, "\"updated\":"); long b = strtol(p + 10, 0, 10);
        CHECK(a >= b, "default sort updated desc");
    }
    CHECK(req("GET", "/api/issues?limit=50&page=10", 0) == 200 && count_items(shim.body) == 50, "page 10 of 50");
    CHECK(req("GET", "/api/issues?limit=50&page=11", 0) == 200 && count_items(shim.body) == 0 && jint(shim.body, "total") == 500, "page past end");
    CHECK(req("GET", "/api/issues?limit=1000", 0) == 200 && jint(shim.body, "limit") == 50 && count_items(shim.body) == 50, "limit clamp 50");
    CHECK(req("GET", "/api/issues?page=0", 0) == 200 && jint(shim.body, "page") == 1, "page 0 -> 1");
    /* status filters partition the set */
    long sum = 0; const char* sts[] = { "backlog", "todo", "in_progress", "review", "done" };
    for (int i = 0; i < 5; i++) {
        char path[64]; snprintf(path, sizeof path, "/api/issues?status=%s", sts[i]);
        CHECK(req("GET", path, 0) == 200, "status filter");
        long t = jint(shim.body, "total"); sum += t;
        char pat[32]; snprintf(pat, sizeof pat, "\"status\":\"%s\"", sts[i]);
        CHECK(count_items(shim.body) == count_substr(shim.body, pat), "all items have status %s", sts[i]);
    }
    CHECK(sum == 500, "status totals sum to 500: %ld", sum);
    CHECK(req("GET", "/api/issues?status=todo,done", 0) == 200, "comma list");
    long td = jint(shim.body, "total");
    req("GET", "/api/issues?status=todo", 0); long t1 = jint(shim.body, "total");
    req("GET", "/api/issues?status=done", 0); long t2 = jint(shim.body, "total");
    CHECK(td == t1 + t2, "todo,done = todo + done");
    CHECK(req("GET", "/api/issues?status=bogus", 0) == 400, "unknown status 400");
    /* project */
    sum = 0;
    for (int p = 1; p <= 3; p++) { char path[64]; snprintf(path, sizeof path, "/api/issues?project=%d&limit=50", p); req("GET", path, 0); sum += jint(shim.body, "total");
        char pat[32]; snprintf(pat, sizeof pat, "\"project\":%d", p); CHECK(count_items(shim.body) == count_substr(shim.body, pat), "project filter items"); }
    CHECK(sum == 500, "project totals sum to 500: %ld", sum);
    /* assignee */
    sum = 0;
    for (int u = 1; u <= 8; u++) { char path[64]; snprintf(path, sizeof path, "/api/issues?assignee=%d&limit=50", u); req("GET", path, 0); sum += jint(shim.body, "total");
        char pat[32]; snprintf(pat, sizeof pat, "\"assignee\":%d", u); CHECK(count_items(shim.body) == count_substr(shim.body, pat), "assignee filter items"); }
    req("GET", "/api/issues?assignee=none", 0); long unassigned = jint(shim.body, "total");
    CHECK(sum + unassigned == 500, "assignee totals + unassigned = 500 (%ld + %ld)", sum, unassigned);
    CHECK(unassigned > 0 && count_substr(shim.body, "\"assignee\":null") == count_items(shim.body), "unassigned render as null");
    /* label, priority */
    req("GET", "/api/issues?label=0&limit=50", 0);
    CHECK(jint(shim.body, "total") > 0 && count_substr(shim.body, "\"labels\":[0") + count_substr(shim.body, ",0]") + count_substr(shim.body, ",0,") >= count_items(shim.body), "label filter");
    sum = 0;
    for (int p = 0; p < 5; p++) { char path[64]; snprintf(path, sizeof path, "/api/issues?priority=%d&limit=50", p); req("GET", path, 0); sum += jint(shim.body, "total");
        char pat[32]; snprintf(pat, sizeof pat, "\"priority\":%d", p); CHECK(count_items(shim.body) == count_substr(shim.body, pat), "priority filter items"); }
    CHECK(sum == 500, "priority totals sum to 500: %ld", sum);
    CHECK(req("GET", "/api/issues?priority=9", 0) == 400, "priority range");
    /* combined */
    req("GET", "/api/issues?project=1&status=done&priority=2&limit=50", 0);
    CHECK(jint(shim.body, "total") >= 0 && count_items(shim.body) == count_substr(shim.body, "\"status\":\"done\""), "combined filter");
    /* sorts */
    CHECK(req("GET", "/api/issues?sort=key&order=asc", 0) == 200 && strstr(shim.body, "\"items\":[{\"id\":1,"), "sort key asc starts at id 1");
    CHECK(req("GET", "/api/issues?sort=key&order=desc", 0) == 200 && strstr(shim.body, "\"items\":[{\"id\":500,"), "sort key desc starts at id 500");
    req("GET", "/api/issues?sort=priority&order=desc", 0);
    CHECK(strstr(shim.body, "\"priority\":4") != 0, "priority desc starts with urgent");
    req("GET", "/api/issues?sort=priority&order=asc", 0);
    { const char* p = strstr(shim.body, "\"priority\":"); CHECK(p && p[11] == '0', "priority asc starts with none"); }
    req("GET", "/api/issues?sort=created&order=asc", 0);
    { const char* p = strstr(shim.body, "\"created\":"); long a = strtol(p + 10, 0, 10); p = strstr(p + 10, "\"created\":"); long b = strtol(p + 10, 0, 10); CHECK(a <= b, "created asc"); }
    CHECK(req("GET", "/api/issues?sort=bogus", 0) == 400, "unknown sort");
    CHECK(req("GET", "/api/issues?order=sideways", 0) == 400, "unknown order");
    /* q uses the index */
    CHECK(req("GET", "/api/issues?q=upload&limit=50", 0) == 200, "list with q");
    long lq = jint(shim.body, "total");
    req("GET", "/api/search?q=upload&limit=50", 0);
    CHECK(lq == jint(shim.body, "total"), "list q total equals search total (%ld)", lq);
}

static void test_get(void) {
    char path[64]; snprintf(path, sizeof path, "/api/issues/%s", last_key);
    CHECK(req("GET", path, 0) == 200, "get by key");
    CHECK(strstr(shim.body, "\"description\":\"") && strstr(shim.body, "\"comments\":[") && strstr(shim.body, "\"activity\":[") && strstr(shim.body, "\"now\":"), "full issue shape");
    char kb[32]; jstr(shim.body, "key", kb, sizeof kb);
    CHECK(strcmp(kb, last_key) == 0, "key round trip");
    long cc = jint(shim.body, "comment_count");
    CHECK(cc == count_substr(shim.body, "\"body\":\""), "comment_count matches comments array (%ld)", cc);
    CHECK(req("GET", "/api/issues/WEB-99999", 0) == 404 && strstr(shim.body, "\"error\":\"not_found\""), "404 unknown issue");
    CHECK(req("GET", "/api/issues/XXX-1", 0) == 404, "404 unknown project");
    CHECK(req("GET", "/api/issues/WEB-abc", 0) == 404, "404 bad id");
    /* key with the wrong project prefix must not resolve */
    snprintf(path, sizeof path, "/api/issues/%s", last_key);
    req("GET", path, 0); long proj = jint(shim.body, "project");
    const char* other = proj == 1 ? "IOS" : "WEB";
    snprintf(path, sizeof path, "/api/issues/%s-%s", other, strchr(last_key, '-') + 1);
    CHECK(req("GET", path, 0) == 404, "wrong project prefix is 404");
    CHECK(req("GET", "/api/nope", 0) == 404, "unknown route");
    CHECK(req("PUT", "/api/issues", 0) == 405, "method not allowed");
    CHECK(req("GET", "/api/auth/login", 0) == 405, "login GET 405");
}

static char created_key[32];
static long done_before;
static void test_create_patch_comment_delete(void) {
    req("GET", "/api/analytics", 0);
    done_before = jint(shim.body, "done");
    shim_reset_events();
    CHECK(req("POST", "/api/issues", "{\"title\":\"\"}") == 400, "empty title");
    CHECK(req("POST", "/api/issues", "{\"description\":\"x\"}") == 400, "missing title");
    CHECK(req("POST", "/api/issues", "{\"title\":\"x\",\"priority\":7}") == 400, "priority range");
    CHECK(req("POST", "/api/issues", "{\"title\":\"x\",\"labels\":[99]}") == 400, "label range");
    CHECK(req("POST", "/api/issues", "{\"title\":\"x\",\"status\":\"nah\"}") == 400, "status name");
    CHECK(req("POST", "/api/issues", "not json") == 400, "malformed");
    CHECK(req("POST", "/api/issues", 0) == 400, "no body");
    {
        static char big[9000]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
        char body[9100]; snprintf(body, sizeof body, "{\"title\":\"ok\",\"description\":\"%s\"}", big);
        CHECK(req("POST", "/api/issues", body) == 400, "oversized body");
        char t[200]; memset(t, 'b', 121); t[121] = 0;
        snprintf(body, sizeof body, "{\"title\":\"%s\"}", t);
        CHECK(req("POST", "/api/issues", body) == 400, "title > 120");
    }
    CHECK(shim.event_n == 0, "no events on rejected creates");
    int st = req("POST", "/api/issues", "{\"title\":\"Zebra quartz \\\"locomotive\\\" overflow\",\"description\":\"The flux capacitor \\u00e9mits smoke\\nwhen idle\",\"project\":2,\"status\":\"todo\",\"priority\":3,\"assignee\":4,\"labels\":[0,\"perf\"],\"estimate\":5}");
    CHECK(st == 201, "create 201, got %d: %s", st, shim.body);
    jstr(shim.body, "key", created_key, sizeof created_key);
    CHECK(strncmp(created_key, "IOS-", 4) == 0, "key uses project prefix: %s", created_key);
    CHECK(strstr(shim.body, "\"title\":\"Zebra quartz \\\"locomotive\\\" overflow\"") != 0, "title escaped on output");
    CHECK(strstr(shim.body, "\"description\":\"The flux capacitor \xc3\xa9mits smoke\\nwhen idle\"") != 0, "description unescaped then re-escaped");
    CHECK(strstr(shim.body, "\"labels\":[0,2]") && strstr(shim.body, "\"estimate\":5") && strstr(shim.body, "\"assignee\":4") && strstr(shim.body, "\"reporter\":2") && strstr(shim.body, "\"status\":\"todo\""), "create fields");
    CHECK(shim.event_n == 1 && strstr(shim.events[0].data, "\"type\":\"issue\"") && strstr(shim.events[0].data, "\"op\":\"create\"") && strstr(shim.events[0].data, created_key) && strstr(shim.events[0].data, "\"actor\":2"), "create event: %s", shim.event_n ? shim.events[0].data : "(none)");
    CHECK(strcmp(shim.events[0].topic, "tessera.events") == 0, "event topic");
    /* index updated */
    CHECK(req("GET", "/api/search?q=zebra", 0) == 200 && jint(shim.body, "total") == 1 && strstr(shim.body, created_key), "search finds new title");
    CHECK(req("GET", "/api/search?q=zebra+quartz", 0) == 200 && jint(shim.body, "total") == 1, "multi token");
    CHECK(req("GET", "/api/search?q=locomot", 0) == 200 && jint(shim.body, "total") == 1, "prefix on last token");
    CHECK(req("GET", "/api/search?q=capacitor", 0) == 200 && jint(shim.body, "total") == 1, "description indexed");
    CHECK(req("GET", "/api/search?q=zebra+wifi", 0) == 200 && jint(shim.body, "total") == 0, "AND semantics");
    req_nocookie("GET", "/api/bootstrap", 0);
    CHECK(jint(shim.body, "total") == total_before_create + 1, "bootstrap total +1");

    /* patch */
    char path[64]; snprintf(path, sizeof path, "/api/issues/%s", created_key);
    shim_reset_events();
    CHECK(req("PATCH", path, "{\"status\":\"done\",\"priority\":4}") == 200, "patch");
    CHECK(strstr(shim.body, "\"status\":\"done\"") && strstr(shim.body, "\"priority\":4"), "patched fields");
    CHECK(shim.event_n == 1 && strstr(shim.events[0].data, "\"op\":\"update\"") && strstr(shim.events[0].data, "\"status\":\"done\""), "update event");
    CHECK(req("PATCH", path, "{\"priority\":4}") == 200 && shim.event_n == 1, "no-op patch emits nothing");
    CHECK(req("PATCH", path, "{\"title\":\"\"}") == 400, "patch validation");
    CHECK(req("PATCH", path, "{\"title\":\"Zebra renamed gyroscope\",\"assignee\":null}") == 200 && strstr(shim.body, "\"assignee\":null"), "patch title + unassign");
    CHECK(req("GET", "/api/search?q=gyroscope", 0) == 200 && jint(shim.body, "total") == 1, "re-indexed after title patch");
    CHECK(req("GET", "/api/search?q=quartz", 0) == 200 && jint(shim.body, "total") == 0, "old title terms removed");
    CHECK(req("GET", path, 0) == 200, "get after patch");
    CHECK(strstr(shim.body, "\"op\":\"update\",\"field\":\"status\",\"from\":\"todo\",\"to\":\"done\"") != 0, "status activity recorded: %s", strstr(shim.body, "\"activity\""));
    CHECK(strstr(shim.body, "\"field\":\"priority\",\"from\":\"high\",\"to\":\"urgent\"") != 0, "priority activity");
    CHECK(strstr(shim.body, "\"op\":\"create\"") != 0, "create activity");
    req("GET", "/api/analytics", 0);
    CHECK(jint(shim.body, "done") == done_before + 1, "analytics resolved count +1 (%ld -> %ld)", done_before, jint(shim.body, "done"));
    CHECK(req("PATCH", "/api/issues/WEB-99999", "{\"status\":\"done\"}") == 404, "patch 404");

    /* comment */
    snprintf(path, sizeof path, "/api/issues/%s/comments", created_key);
    shim_reset_events();
    CHECK(req("POST", path, "{\"body\":\"\"}") == 400, "empty comment");
    CHECK(req("POST", path, "{}") == 400, "missing body");
    st = req("POST", path, "{\"body\":\"Pangolin confirms the overflow on staging\"}");
    CHECK(st == 201, "comment 201");
    long cid = jint(shim.body, "id");
    CHECK(cid > 0 && strstr(shim.body, "\"author\":2") && strstr(shim.body, "\"body\":\"Pangolin confirms") && strstr(shim.body, "\"created\":") && strstr(shim.body, "\"issue\":"), "comment shape");
    CHECK(shim.event_n == 1 && strstr(shim.events[0].data, "\"type\":\"comment\"") && strstr(shim.events[0].data, "\"author\":2") && strstr(shim.events[0].data, created_key), "comment event");
    CHECK(req("GET", "/api/search?q=pangolin", 0) == 200 && jint(shim.body, "total") == 1, "comment indexed");
    snprintf(path, sizeof path, "/api/issues/%s", created_key);
    req("GET", path, 0);
    CHECK(jint(shim.body, "comment_count") == 1 && strstr(shim.body, "\"op\":\"comment\""), "comment visible + activity");
    req("GET", "/api/activity?limit=5", 0);
    CHECK(count_items(shim.body) == 5 && strstr(shim.body, "\"op\":\"comment\""), "activity feed newest first");
    CHECK(req("GET", "/api/activity?limit=1000", 0) == 200 && count_items(shim.body) <= 200, "activity limit clamp");

    /* delete */
    shim_reset_events();
    CHECK(req("DELETE", path, 0) == 204, "delete 204");
    CHECK(shim.event_n == 1 && strstr(shim.events[0].data, "\"op\":\"delete\""), "delete event");
    CHECK(req("GET", path, 0) == 404, "deleted is 404");
    CHECK(req("DELETE", path, 0) == 404, "double delete 404");
    CHECK(req("GET", "/api/search?q=zebra", 0) == 200 && jint(shim.body, "total") == 0, "search no longer finds deleted");
    CHECK(req("GET", "/api/search?q=pangolin", 0) == 200 && jint(shim.body, "total") == 0, "deleted comments gone from search");
    req("GET", "/api/issues?project=2&limit=50&sort=key&order=desc", 0);
    CHECK(strstr(shim.body, created_key) == 0, "deleted not listed");
    req_nocookie("GET", "/api/bootstrap", 0);
    CHECK(jint(shim.body, "total") == total_before_create, "bootstrap total back");
    req("GET", "/api/analytics", 0);
    CHECK(jint(shim.body, "done") == done_before, "analytics back");
}

static void test_search_ranking(void) {
    CHECK(req("POST", "/api/issues", "{\"title\":\"Other thing entirely\",\"description\":\"nebulizer calibration broken again\"}") == 201, "desc issue");
    char kb[32]; jstr(shim.body, "key", kb, sizeof kb);
    CHECK(req("POST", "/api/issues", "{\"title\":\"Nebulizer calibration\"}") == 201, "title issue");
    char kt[32]; jstr(shim.body, "key", kt, sizeof kt);
    CHECK(req("GET", "/api/search?q=nebulizer", 0) == 200 && jint(shim.body, "total") == 2, "both found");
    CHECK(strstr(shim.body, kt) < strstr(shim.body, kb), "title hit outranks description hit");
    {
        const char* a = strstr(shim.body, "\"score\":"); long s1 = strtol(a + 8, 0, 10);
        const char* b = strstr(a + 8, "\"score\":"); long s2 = strtol(b + 8, 0, 10);
        CHECK(s1 > s2, "scores descending %ld > %ld", s1, s2);
    }
    CHECK(req("GET", "/api/search?q=nebul", 0) == 200 && jint(shim.body, "total") == 2, "prefix");
    CHECK(req("GET", "/api/search?q=NEBULIZER+calib", 0) == 200 && jint(shim.body, "total") == 2, "case + prefix");
    CHECK(strstr(shim.body, "\"scanned\":") && strstr(shim.body, "\"took_us\":") && strstr(shim.body, "\"items\":[{\"key\":\"") && strstr(shim.body, "\"score\":"), "search shape");
    CHECK(req("GET", "/api/search?q=", 0) == 200 && jint(shim.body, "total") == 0, "empty query");
    CHECK(req("GET", "/api/search?q=the", 0) == 200, "stopword-only query");
    CHECK(req("GET", "/api/search?q=upload&limit=3", 0) == 200 && count_items(shim.body) == 3 && jint(shim.body, "total") > 3, "search limit");
    CHECK(req("GET", "/api/search?q=%E2%9C%93+wifi", 0) == 200, "url encoded non-ascii");
    /* clean up so later counts stay simple */
    char path[64];
    snprintf(path, sizeof path, "/api/issues/%s", kb); req("DELETE", path, 0);
    snprintf(path, sizeof path, "/api/issues/%s", kt); req("DELETE", path, 0);
}

static void test_board_analytics_stats(void) {
    CHECK(req("GET", "/api/board", 0) == 200, "board all");
    CHECK(strstr(shim.body, "\"columns\":{\"backlog\":[") && strstr(shim.body, "\"limit\":40") && strstr(shim.body, "\"truncated\":{"), "board shape");
    {
        const char* p = strstr(shim.body, "\"backlog\":[");
        int n = 0, depth = 0;
        for (p += 11; *p; p++) { if (*p == '{') { if (!depth) n++; depth++; } else if (*p == '}') depth--; else if (*p == ']' && !depth) break; }
        CHECK(n == 40, "backlog column capped at 40 (got %d)", n);
        CHECK(strstr(shim.body, "\"truncated\":{") && strstr(strstr(shim.body, "\"truncated\":{"), "\"backlog\":true"), "truncated flag");
        /* priority desc then updated desc within the column */
        const char* q = strstr(shim.body, "\"backlog\":[");
        const char* a = strstr(q, "\"priority\":"); long pa = strtol(a + 11, 0, 10);
        const char* b = strstr(a + 11, "\"priority\":"); long pb = strtol(b + 11, 0, 10);
        CHECK(pa >= pb, "board column priority desc");
    }
    CHECK(req("GET", "/api/board?project=3", 0) == 200 && count_substr(shim.body, "\"project\":1") == 0, "board project filter");
    CHECK(req("GET", "/api/board?project=9", 0) == 400, "board bad project");

    CHECK(req("GET", "/api/analytics?days=14", 0) == 200, "analytics");
    CHECK(array_len(shim.body, "created_per_day") == 14 && array_len(shim.body, "resolved_per_day") == 14, "arrays length = days");
    CHECK(req("GET", "/api/analytics?days=30&project=1", 0) == 200 && array_len(shim.body, "created_per_day") == 30, "days=30");
    CHECK(req("GET", "/api/analytics?days=1000", 0) == 200 && array_len(shim.body, "created_per_day") == 90, "days clamp 90");
    CHECK(array_len(shim.body, "by_priority") == 5 && strstr(shim.body, "\"by_assignee\":[{\"user\":1,\"open\":") && strstr(shim.body, "\"cycle_time_hours\":{\"p50\":") && strstr(shim.body, "\"open_age_buckets\":{\"lt1d\":") && strstr(shim.body, "\"took_us\":"), "analytics shape");
    CHECK(count_substr(shim.body, "\"user\":") == 8, "8 assignees");
    CHECK(jint(shim.body, "p90") >= jint(shim.body, "p50") && jint(shim.body, "p50") > 0, "cycle percentiles");
    {
        long bs = jint(strstr(shim.body, "\"by_status\""), "backlog") + jint(strstr(shim.body, "\"by_status\""), "todo") + jint(strstr(shim.body, "\"by_status\""), "in_progress") + jint(strstr(shim.body, "\"by_status\""), "review") + jint(strstr(shim.body, "\"by_status\""), "done");
        CHECK(bs == 500, "by_status sums to 500: %ld", bs);
        const char* ob = strstr(shim.body, "open_age_buckets");
        long ages = jint(ob, "lt1d") + jint(ob, "lt7d") + jint(ob, "lt30d") + jint(ob, "gte30d");
        CHECK(ages == 500 - jint(strstr(shim.body, "\"by_status\""), "done"), "age buckets cover open issues (%ld)", ages);
    }

    CHECK(req_nocookie("GET", "/api/stats", 0) == 200, "stats public");
    CHECK(jint(shim.body, "issues") == 500 && jint(shim.body, "comments") >= 1000 && jint(shim.body, "users") == 8 && jint(shim.body, "sessions") == 1, "stats counts: %s", shim.body);
    CHECK(strstr(shim.body, "\"index\":{\"terms\":") && strstr(shim.body, "\"postings\":") && strstr(shim.body, "\"memory\":{\"used\":") && strstr(shim.body, "\"limit\":") &&
          strstr(shim.body, "\"requests\":{\"total\":") && strstr(shim.body, "\"errors\":") && strstr(shim.body, "\"avg_us\":") && strstr(shim.body, "\"p99_us\":") && strstr(shim.body, "\"last_us\":") &&
          strstr(shim.body, "\"uptime_ms\":") && strstr(shim.body, "\"now\":"), "stats shape");
    CHECK(jint(shim.body, "total") > 50 && jint(shim.body, "errors") > 10, "request counters");
}

static void test_keyboard(void) {
    char path[64]; snprintf(path, sizeof path, "/api/issues/%s", last_key);
    req("GET", path, 0);   /* most recently viewed */
    long cc = jint(shim.body, "comment_count");
    shim_reset_events();
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"h\"}") == 0, "key event accepted");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"x\"}") == 0, "key event accepted");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"\\b\"}") == 0, "backspace accepted");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"i\"}") == 0, "key event accepted");
    CHECK(shim.event_n == 4, "4 hud events, got %d", shim.event_n);
    CHECK(strcmp(shim.events[0].data, "{\"type\":\"hud\",\"text\":\"h\"}") == 0, "hud text h: %s", shim.events[0].data);
    CHECK(strcmp(shim.events[1].data, "{\"type\":\"hud\",\"text\":\"hx\"}") == 0, "hud text hx");
    CHECK(strcmp(shim.events[2].data, "{\"type\":\"hud\",\"text\":\"h\"}") == 0, "hud text after backspace");
    CHECK(strcmp(shim.events[3].data, "{\"type\":\"hud\",\"text\":\"hi\"}") == 0, "hud text hi");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"\\n\"}") == 0, "enter accepted");
    CHECK(find_event("\"type\":\"comment\"") && strstr(find_event("\"type\":\"comment\"")->data, "\"author\":0") && strstr(find_event("\"type\":\"comment\"")->data, last_key), "enter emits comment event by hud user");
    CHECK(find_event("\"text\":\"\"") != 0, "hud cleared after enter");
    req("GET", path, 0);
    CHECK(jint(shim.body, "comment_count") == cc + 1 && strstr(shim.body, "\"author\":0,\"body\":\"hi\""), "comment by user 0 with body hi");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"\\n\"}") == 0, "enter on empty note is a no-op");
    CHECK(hx_event("other.topic", "{\"t\":\"key\",\"c\":\"a\"}") == 0, "other topics ignored");
    CHECK(hx_event("tessera.input", "garbage") < 0, "malformed input rejected");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"\\u00e9\"}") == 0, "utf-8 key");
    CHECK(hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"Backspace\"}") == 0, "named backspace");
    CHECK(strcmp(shim.events[shim.event_n - 1].data, "{\"type\":\"hud\",\"text\":\"\"}") == 0, "utf-8 backspace removes whole char");
    CHECK(shim.oversized_events == 0, "no event over 200 bytes");
    /* flood the note: must stay bounded and never trap */
    for (int i = 0; i < 400; i++) hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"ab\"}");
    CHECK(shim.oversized_events == 0, "no event over 200 bytes after flood");
    hx_event("tessera.input", "{\"t\":\"key\",\"c\":\"\\n\"}");
}

static void test_tick_expiry(void) {
    CHECK(req("GET", "/api/me", 0) == 200, "session alive");
    k32_tick(0);
    CHECK(req("GET", "/api/me", 0) == 200, "tick keeps fresh session");
    shim.time_offset_ms += 25ll * 3600 * 1000;
    k32_tick(0);
    CHECK(req("GET", "/api/me", 0) == 401, "session expired after 25h idle");
    req_nocookie("GET", "/api/stats", 0);
    CHECK(jint(shim.body, "sessions") == 0, "no sessions after expiry");
    /* 32 sessions cap: 33rd login evicts the oldest */
    char first[64] = { 0 };
    for (int i = 0; i < 33; i++) {
        req_nocookie("POST", "/api/auth/login", "{\"username\":\"sana\",\"password\":\"demo\"}");
        if (i == 0) snprintf(first, sizeof first, "tsid=%.16s", shim.headers[0].value + 5);
        shim.time_offset_ms += 10;
    }
    req_nocookie("GET", "/api/stats", 0);
    CHECK(jint(shim.body, "sessions") == 32, "sessions capped at 32: %ld", jint(shim.body, "sessions"));
    CHECK(hx_request("GET", "/api/me", first, 0) == 401, "oldest session evicted");
}

static void test_malformed_requests(void) {
    uint32_t p = k32_alloc(64);
    memcpy(tessera_resolve(p), "GARBAGE", 7);
    shim_reset_response();
    CHECK(k32_handle(p, 7) == 400 && shim.begin_calls == 1, "garbage request is 400 with one response");
    p = k32_alloc(8); memcpy(tessera_resolve(p), "GET\n", 4);
    shim_reset_response();
    CHECK(k32_handle(p, 4) == 400 && shim.begin_calls == 1, "no path");
    p = k32_alloc(8);
    shim_reset_response();
    CHECK(k32_handle(p, 0) == 400 && shim.begin_calls == 1, "empty request");
    CHECK(req("GET", "/api/issues?limit=abc", 0) == 200, "unparseable limit falls back to default");
    CHECK(req("GET", "/api/issues?q=%", 0) == 200, "broken percent encoding");
    CHECK(req("GET", "/api/issues/", 0) == 200, "trailing slash tolerated");
    CHECK(req("POST", "/api/issues", "{\"title\":\"nested\",\"meta\":{\"a\":[1,{\"b\":2}]}}") == 201, "nested objects skipped");
    char kb[32]; jstr(shim.body, "key", kb, sizeof kb);
    char path[64]; snprintf(path, sizeof path, "/api/issues/%s", kb);
    req("DELETE", path, 0);
    /* 16 KB request at the host limit */
    {
        static char big[16384 + 64];
        int n = snprintf(big, sizeof big, "GET /api/issues?x=");
        memset(big + n, 'y', 16384 - n - 64); n = 16384 - 64;
        n += snprintf(big + n, sizeof big - n, "\nCookie: %s\n\n", cookie);
        uint32_t bp = k32_alloc((uint32_t)n);
        memcpy(tessera_resolve(bp), big, (size_t)n);
        shim_reset_response();
        CHECK(k32_handle(bp, (uint32_t)n) == 200 && shim.begin_calls == 1, "16 KB request handled");
    }
}

/* ---- incremental seeding: runs right after k32_init, before the set is complete ---- */
static void test_incremental_seed(void) {
    tessera_layout L; tessera_layout_get(&L);
    CHECK(L.issue_count == 10, "k32_init seeds only SEED_CHUNK issues (%u)", L.issue_count);
    CHECK(req_nocookie("GET", "/api/stats", 0) == 200 && strstr(shim.body, "\"seeding\":{\"done\":10,\"total\":500}"), "stats exposes seeding progress: %s", shim.body);
    CHECK(req_nocookie("GET", "/api/bootstrap", 0) == 200 && jint(shim.body, "total") == 10, "bootstrap counts the partial set");
    cookie[0] = 0;
    req_nocookie("POST", "/api/auth/login", "{\"username\":\"mara\",\"password\":\"demo\"}");
    snprintf(cookie, sizeof cookie, "tsid=%.16s", shim.headers[0].value + 5);
    CHECK(req("GET", "/api/issues?limit=50", 0) == 200 && jint(shim.body, "total") == 10 && count_items(shim.body) == 10, "list works mid-seed");
    CHECK(req("GET", "/api/board", 0) == 200, "board works mid-seed");
    /* search over the partial index: the first word of issue 1's title must be found */
    req("GET", "/api/issues?sort=key&order=asc&limit=1", 0);
    char title[160] = { 0 }; jstr(shim.body, "title", title, sizeof title);
    char word[32] = { 0 }; sscanf(title, "%31[A-Za-z0-9]", word);
    char path[128]; snprintf(path, sizeof path, "/api/search?q=%s", word);
    CHECK(req("GET", path, 0) == 200 && jint(shim.body, "total") >= 1 && strstr(shim.body, "\"key\":\"") && strstr(shim.body, "-1\""), "search works mid-seed (%s -> %ld)", word, jint(shim.body, "total"));
    /* a create mid-seed takes the next id; seeding continues after it */
    CHECK(req("POST", "/api/issues", "{\"title\":\"Midseed xylophone calibration\",\"project\":1}") == 201 && jint(shim.body, "id") == 11, "create mid-seed gets id 11: %s", shim.body);
    shim_reset_events();
    int rc = k32_tick(0);
    CHECK(rc == 1, "tick returns 1 while seeding remains (%d)", rc);
    tessera_layout_get(&L);
    CHECK(L.issue_count == 21, "tick seeded one more chunk (%u)", L.issue_count);
    CHECK(shim.event_n == 1 && strstr(shim.events[0].data, "\"type\":\"issue\"") && strstr(shim.events[0].data, "\"op\":\"create\"") && strstr(shim.events[0].data, "-21\""), "chunk emits one create event: %s", shim.event_n ? shim.events[0].data : "(none)");
    CHECK(req_nocookie("GET", "/api/stats", 0) == 200 && strstr(shim.body, "\"seeding\":{\"done\":20,\"total\":500}"), "progress after one tick: %s", strstr(shim.body, "\"seeding\""));
    CHECK(req("GET", "/api/search?q=xylophone", 0) == 200 && jint(shim.body, "total") == 1, "mid-seed create is indexed");
    int ticks = 1;
    while ((rc = k32_tick(0)) > 0) ticks++;
    CHECK(rc == 0 && ticks == 48, "48 ticks return 1 after init did chunk 1 of 50 (%d)", ticks);
    CHECK(k32_tick(0) == 0, "idle tick returns 0");
    tessera_layout_get(&L);
    CHECK(L.issue_count == 501 && L.comment_count == 1000, "full set + the mid-seed issue (%u/%u)", L.issue_count, L.comment_count);
    CHECK(req_nocookie("GET", "/api/stats", 0) == 200 && strstr(shim.body, "\"seeding\"") == 0, "seeding key omitted once done");
    CHECK(req("GET", "/api/search?q=xylophone", 0) == 200 && jint(shim.body, "total") == 1, "mid-seed create survives the rest of the seeding");
    CHECK(req("GET", "/api/activity?limit=200", 0) == 200 && count_items(shim.body) == 200, "activity ring seeded in the final chunk");
    CHECK(L.postings_leaked == 0, "posting region packed after seeding (%u leaked)", L.postings_leaked);
    rc = index_verify();
    CHECK(rc == 0, "index matches a fresh tokenization after incremental seeding (rc=%d)", rc);
}

/* ---- top-k pages must equal the pages of a full sort ---- */
typedef struct { long id, priority, created, updated; char status[16]; } ref_issue;
static ref_issue ref[2048]; static int ref_n;
static int ref_key, ref_desc;
static int ref_cmp(const void* a, const void* b) {
    const ref_issue* x = (const ref_issue*)a; const ref_issue* y = (const ref_issue*)b;
    long r = 0;
    switch (ref_key) {
        case 0: r = x->updated - y->updated; break;
        case 1: r = x->created - y->created; break;
        case 2: r = x->priority - y->priority; if (!r) r = x->updated - y->updated; break;
        case 3: r = 0; break;
        case 4: r = y->priority - x->priority; if (!r) r = y->updated - x->updated; if (!r) r = y->id - x->id; return r < 0 ? -1 : r > 0;
    }
    if (!r) r = x->id - y->id;
    if (ref_desc) r = -r;
    return r < 0 ? -1 : r > 0;
}
static int parse_ids(const char* body, const char* arr_key, long* out, int cap) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":[", arr_key);
    const char* p = strstr(body, pat);
    if (!p) return -1;
    p += strlen(pat);
    int n = 0, depth = 0;
    for (; *p; p++) {
        if (*p == '{') { if (depth == 0 && n < cap && strncmp(p, "{\"id\":", 6) == 0) out[n++] = strtol(p + 6, 0, 10); depth++; }
        else if (*p == '}') depth--;
        else if (*p == ']' && depth == 0) break;
    }
    return n;
}
static void test_topk_equivalence(void) {
    /* collect every issue once (sort=key is id order; 50 per page) */
    ref_n = 0;
    for (int page = 1; page <= 20; page++) {
        char path[96]; snprintf(path, sizeof path, "/api/issues?sort=key&order=asc&limit=50&page=%d", page);
        if (req("GET", path, 0) != 200 || count_items(shim.body) == 0) break;
        const char* p = shim.body;
        while ((p = strstr(p, "{\"id\":")) && ref_n < 2048) {
            ref_issue* r = &ref[ref_n++];
            r->id = strtol(p + 6, 0, 10);
            r->priority = jint(p, "priority"); r->created = jint(p, "created"); r->updated = jint(p, "updated");
            jstr(p, "status", r->status, sizeof r->status);
            p += 6;
        }
    }
    req("GET", "/api/issues?limit=1", 0);
    CHECK(ref_n == jint(shim.body, "total"), "reference holds every issue (%d vs %ld)", ref_n, jint(shim.body, "total"));
    static const char* keys[] = { "updated", "created", "priority", "key" };
    static const int limits[] = { 1, 7, 25, 50 };
    static const int pages[] = { 1, 2, 3, 10, 21 };
    long got[64];
    int mismatches = 0, compared = 0;
    for (int k = 0; k < 4; k++) for (int d = 0; d < 2; d++) {
        ref_key = k; ref_desc = d;
        qsort(ref, ref_n, sizeof ref[0], ref_cmp);
        for (int li = 0; li < 4; li++) for (int pi = 0; pi < 5; pi++) {
            int limit = limits[li], page = pages[pi];
            char path[128]; snprintf(path, sizeof path, "/api/issues?sort=%s&order=%s&limit=%d&page=%d", keys[k], d ? "desc" : "asc", limit, page);
            int st = req("GET", path, 0);
            int n = parse_ids(shim.body, "items", got, 64);
            long start = (long)(page - 1) * limit, expect_n = start >= ref_n ? 0 : (ref_n - start < limit ? ref_n - start : limit);
            int ok = st == 200 && n == expect_n && jint(shim.body, "total") == ref_n;
            for (int i = 0; ok && i < n; i++) if (got[i] != ref[start + i].id) ok = 0;
            compared++;
            if (!ok) { mismatches++; fprintf(stderr, "  topk mismatch: %s (n=%d expect %ld)\n", path, n, expect_n); }
        }
    }
    CHECK(mismatches == 0, "top-k pages equal full-sort pages (%d/%d page requests matched)", compared - mismatches, compared);
    /* with a filter: status=done, default sort (updated desc) */
    {
        ref_issue done[2048]; int dn = 0;
        for (int i = 0; i < ref_n; i++) if (strcmp(ref[i].status, "done") == 0) done[dn++] = ref[i];
        ref_key = 0; ref_desc = 1; qsort(done, dn, sizeof done[0], ref_cmp);
        int ok = 1;
        for (int page = 1; page <= 3 && ok; page++) {
            char path[96]; snprintf(path, sizeof path, "/api/issues?status=done&limit=25&page=%d", page);
            req("GET", path, 0);
            int n = parse_ids(shim.body, "items", got, 64);
            if (jint(shim.body, "total") != dn) ok = 0;
            for (int i = 0; ok && i < n; i++) if (got[i] != done[(page - 1) * 25 + i].id) ok = 0;
        }
        CHECK(ok, "filtered top-k pages equal full-sort pages (done=%d)", dn);
    }
    /* board columns: top 40 by priority desc, updated desc, id desc */
    {
        ref_key = 4; ref_desc = 0; qsort(ref, ref_n, sizeof ref[0], ref_cmp);
        CHECK(req("GET", "/api/board", 0) == 200, "board");
        static const char* sts[] = { "backlog", "todo", "in_progress", "review", "done" };
        int ok = 1;
        for (int s = 0; s < 5 && ok; s++) {
            int n = parse_ids(shim.body, sts[s], got, 64);
            int j = 0;
            for (int i = 0; i < ref_n && j < n; i++) if (strcmp(ref[i].status, sts[s]) == 0) { if (ref[i].id != got[j]) { ok = 0; break; } j++; }
            if (j != n || n > 40) ok = 0;
        }
        CHECK(ok, "board columns equal the top 40 of a full per-column sort");
    }
    /* search: a smaller limit is a prefix of a larger one, and the order is score desc, updated desc, key desc */
    {
        static const char* qs[] = { "upload", "feed", "cache+key", "th" };
        int ok = 1;
        for (int qi = 0; qi < 4 && ok; qi++) {
            char path[96]; snprintf(path, sizeof path, "/api/search?q=%s&limit=50", qs[qi]);
            req("GET", path, 0);
            long score[64], upd[64], ids[64]; int n = 0;
            for (const char* p = shim.body; (p = strstr(p, "{\"key\":\"")) && n < 64; p += 8) {
                ids[n] = strtol(strchr(p + 8, '-') + 1, 0, 10); score[n] = jint(p, "score"); upd[n] = jint(p, "updated"); n++;
            }
            for (int i = 1; i < n; i++) {
                if (score[i] > score[i - 1]) ok = 0;
                else if (score[i] == score[i - 1] && upd[i] > upd[i - 1]) ok = 0;
                else if (score[i] == score[i - 1] && upd[i] == upd[i - 1] && ids[i] > ids[i - 1]) ok = 0;
            }
            snprintf(path, sizeof path, "/api/search?q=%s&limit=5", qs[qi]);
            req("GET", path, 0);
            int m = 0;
            for (const char* p = shim.body; (p = strstr(p, "{\"key\":\"")) && m < 5; p += 8) { if (strtol(strchr(p + 8, '-') + 1, 0, 10) != ids[m]) ok = 0; m++; }
            if (m != (n < 5 ? n : 5)) ok = 0;
        }
        CHECK(ok, "search top-k ordering and prefix property");
    }
}

int main(void) {
    shim.quiet = 1;
    srand(42);
    int rc = k32_init();
    CHECK(rc == 0, "k32_init rc=%d", rc);
    if (rc != 0) return 1;
    test_incremental_seed();

    /* fresh module state for the data-set tests: init again and let the seeding finish */
    rc = k32_init();
    CHECK(rc == 0, "second k32_init rc=%d", rc);
    int ticks = hx_seed_complete();
    CHECK(ticks == 49, "500 issues: init + 49 tick calls of SEED_CHUNK=10 (%d)", ticks);
    tessera_layout L; tessera_layout_get(&L);
    CHECK(L.issue_count == 500 && L.comment_count == 1000, "seed counts %u/%u", L.issue_count, L.comment_count);
    CHECK(L.issue_cap == 1000, "MAX_ISSUES default = seed + 500 (%u)", L.issue_cap);
    CHECK(L.term_count > 100 && L.posting_count > 5000, "index populated: %u terms, %u postings", L.term_count, L.posting_count);
    rc = index_verify();
    CHECK(rc == 0, "index verified after seeding (rc=%d)", rc);
    rc = store_verify_counts();
    CHECK(rc == 0, "stored comment counters equal a recount after seeding (rc=%d)", rc);

    test_auth();
    test_bootstrap();
    test_list();
    test_get();
    test_create_patch_comment_delete();
    test_search_ranking();
    test_board_analytics_stats();
    test_topk_equivalence();
    test_keyboard();
    test_malformed_requests();
    test_tick_expiry();

    /* index integrity after every mutation above: postings match a fresh tokenization, none reference deleted issues */
    rc = index_verify();
    CHECK(rc == 0, "index verified after mutations (rc=%d)", rc);
    rc = store_verify_counts();
    CHECK(rc == 0, "stored comment counters equal a recount after mutations (rc=%d)", rc);
    req_nocookie("GET", "/api/stats", 0);
    CHECK(shim.oversized_events == 0, "all events under 200 bytes");
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
