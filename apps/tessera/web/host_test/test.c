/* Native tests for tessera-web: path mapping, client-route fallback, cache
 * headers and refusals, against the bundle build.py wrote. */
#define _POSIX_C_SOURCE 200809L
#include "../../common/k32_host.h"
#include "../../common/host/shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* web_resolve(uint32_t ptr);

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static const char* header(const char* name) {
    for (int i = 0; i < shim.header_n; i++) if (!strcmp(shim.headers[i].name, name)) return shim.headers[i].value;
    return "";
}
static int get(const char* method, const char* path) {
    char block[512];
    int n = snprintf(block, sizeof block, "%s %s\nCookie: tsid=0123456789abcdef\n\n", method, path);
    uint32_t p = k32_alloc((uint32_t)n);
    if (!p) return -1;
    memcpy(web_resolve(p), block, (size_t)n);
    shim_reset_response();
    int st = k32_handle(p, (uint32_t)n);
    CHECK(shim.begin_calls == 1 && shim.end_calls == 1 && st == shim.status, "%s %s: one response with the returned status", method, path);
    return st;
}
static void expect_asset(const char* path, const char* file, const char* cache) {
    int st = get("GET", path);
    CHECK(st == 200 && !strcmp(shim.asset_path, file) && !strcmp(header("Cache-Control"), cache),
          "GET %s -> %d %s [%s], want %s [%s]", path, st, shim.asset_path, header("Cache-Control"), file, cache);
}

int main(int argc, char** argv) {
    shim.quiet = 1;
    snprintf(shim.asset_root, sizeof shim.asset_root, "%s", argc > 1 ? argv[1] : "../../../images/web/tessera-web");
    CHECK(k32_init() == 0, "init");
    expect_asset("/", "index.html", "no-cache");
    expect_asset("/index.html", "index.html", "no-cache");
    expect_asset("/app.js?v=0123abcd", "app.js", "public, max-age=31536000, immutable");
    expect_asset("/styles.css?v=9", "styles.css", "public, max-age=31536000, immutable");
    expect_asset("/app.js", "app.js", "public, max-age=3600");
    expect_asset("/board", "index.html", "no-cache");
    expect_asset("/issue/WEB-42", "index.html", "no-cache");
    expect_asset("/settings/infra/", "index.html", "no-cache");
    expect_asset("/issues?status=todo", "index.html", "no-cache");
    CHECK(!strcmp(shim.content_type, "text/html; charset=utf-8"), "html content type: %s", shim.content_type);
    CHECK(get("GET", "/missing.js") == 404 && shim.asset_calls > 0, "a file the bundle lacks is 404");
    CHECK(get("GET", "/../secret.txt") == 404, "parent references refused");
    CHECK(get("GET", "/a//b.js") == 404, "empty path segments refused");
    CHECK(get("GET", "/%2e%2e/x") == 404, "encoded characters refused");
    CHECK(get("GET", "/api/issues") == 404 && get("GET", "/events") == 404, "API paths are not client routes");
    CHECK(get("POST", "/") == 405 && get("DELETE", "/app.js") == 405, "only GET and HEAD");
    CHECK(get("HEAD", "/") == 200, "HEAD");
    CHECK(k32_alloc(0) == 0 && k32_alloc(1u << 16) == 0 && k32_alloc(2048) != 0, "request blocks stay under the cap");
    uint32_t p = k32_alloc(16);
    memcpy(web_resolve(p), "GARBAGE", 7);
    shim_reset_response();
    CHECK(k32_handle(p, 7) == 400 && shim.begin_calls == 1, "malformed request");
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
