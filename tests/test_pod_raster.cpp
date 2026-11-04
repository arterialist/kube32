// The pods' software-rasterizer imports (k32_raster_columns, k32_raster_spans)
// and the display blits (k32_display_blit, k32_display_blit_pal): the kernels
// against plain reference loops, band by band; the imports as a pod calls
// them: results, records the kernel refuses, operands outside the linear
// memory, texels read from a blob in place, a blob that is not mapped, the
// gas they charge; and the blits through the platform hook, with the
// capability firewall's grant.
#include "runtime/pod_raster.hpp"
#include "runtime/pod_linalg.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/container_lifecycle.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_blobs.hpp"
#include "storage/blob_store.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/hal_types.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace kube32::runtime;

static uint32_t s_seed = 777;
static uint32_t rnd() { return s_seed = s_seed * 1664525u + 1013904223u; }

static void st32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void st16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// ---- an arena: the pod's memory as the kernels see it through a resolver ----
struct Arena {
    std::vector<uint8_t> bytes;
    static const uint8_t* resolve(void* ctx, uint32_t addr, uint32_t len) {
        const Arena& a = *static_cast<const Arena*>(ctx);
        return addr <= a.bytes.size() && len <= a.bytes.size() - addr ? a.bytes.data() + addr : nullptr;
    }
};

// ---- reference loops: the engine's arithmetic written plainly ----
constexpr uint32_t kW = 64, kH = 40;

struct Col {
    uint32_t src, index;
    int32_t iscale, texturemid, scale;
    uint32_t x;
    uint8_t a, b, light, kind;
};
static void pack(uint8_t* r, const Col& c) {
    st32(r, c.src); st32(r + 4, c.iscale); st32(r + 8, c.texturemid); st32(r + 12, c.scale);
    st16(r + 16, c.x); st16(r + 18, c.index); r[20] = c.a; r[21] = c.b; r[22] = c.light; r[23] = c.kind;
}

static void ref_solid(std::vector<uint8_t>& frame, const Col& c, const uint8_t* texels, const uint8_t* cmap, const uint8_t* trans,
                      int32_t centery, int32_t yl, int32_t yh, int32_t texturemid) {
    for (int32_t y = yl; y <= yh; y++) {
        const int64_t frac = static_cast<int64_t>(texturemid) + static_cast<int64_t>(y - centery) * c.iscale;
        const uint32_t f = static_cast<uint32_t>(frac);   // the engine's fixed_t wraps at 32 bits
        if (y < 0 || y >= static_cast<int32_t>(kH)) continue;
        uint8_t t;
        if (trans) {
            if ((f >> 16) >= 128) continue;
            t = trans[texels[f >> 16]];
        } else {
            t = texels[(f >> 16) & 127];
        }
        frame[y * kW + c.x] = cmap[c.light * 256 + t];
    }
}

static void ref_column(std::vector<uint8_t>& frame, const Arena& a, const Col& c, const uint8_t* cmap, const uint8_t* trans, int32_t centery) {
    if (c.kind == 0) {
        const uint32_t off = ld32(a.bytes.data() + c.src + 4 * c.index);
        ref_solid(frame, c, a.bytes.data() + off, cmap, trans, centery, c.a, c.b, c.texturemid);
        return;
    }
    uint32_t column;
    if (c.kind == 1) column = c.src + ld32(a.bytes.data() + c.src + 8 + 4 * c.index);
    else column = ld32(a.bytes.data() + c.src + 4 * c.index) - 3;
    const int32_t sprtop = (centery << 16) - static_cast<int32_t>((static_cast<int64_t>(c.texturemid) * c.scale) >> 16);
    const int32_t ceilingclip = c.a - 2, floorclip = c.b - 2;
    for (const uint8_t* post = a.bytes.data() + column; *post != 0xff; post += post[1] + 4) {
        const int32_t topscreen = sprtop + c.scale * post[0], bottomscreen = topscreen + c.scale * post[1];
        int32_t yl = (topscreen + 65535) >> 16, yh = (bottomscreen - 1) >> 16;
        if (yh >= floorclip) yh = floorclip - 1;
        if (yl <= ceilingclip) yl = ceilingclip + 1;
        if (yl <= yh) ref_solid(frame, c, post + 3, cmap, trans, centery, yl, yh, c.texturemid - (post[0] << 16));
    }
}

struct Span {
    uint32_t src;
    int32_t xfrac, yfrac, xstep, ystep;
    uint32_t x1, x2;
    uint8_t y, light;
};
static void pack(uint8_t* r, const Span& s) {
    st32(r, s.src); st32(r + 4, s.xfrac); st32(r + 8, s.yfrac); st32(r + 12, s.xstep); st32(r + 16, s.ystep);
    st16(r + 20, s.x1); st16(r + 22, s.x2); r[24] = s.y; r[25] = s.light; r[26] = r[27] = 0;
}
static void ref_span(std::vector<uint8_t>& frame, const Arena& a, const Span& s, const uint8_t* cmap) {
    // the texel index from the 6.10 position of each pixel, computed from the start without packing
    for (uint32_t x = s.x1; x <= s.x2; x++) {
        const uint32_t k = x - s.x1;
        const uint32_t px = ((static_cast<uint32_t>(s.xfrac) << 10) & 0xffff0000u), py = (static_cast<uint32_t>(s.yfrac) >> 6) & 0xffffu;
        const uint32_t sx = ((static_cast<uint32_t>(s.xstep) << 10) & 0xffff0000u), sy = (static_cast<uint32_t>(s.ystep) >> 6) & 0xffffu;
        // x and y parts step separately; a carry from y into x is part of the engine's packed arithmetic
        const uint32_t position = (px | py) + (sx | sy) * k;
        const uint32_t spot = (position >> 26) | ((position >> 4) & 0x0fc0u);
        frame[s.y * kW + x] = cmap[s.light * 256 + a.bytes[s.src + spot]];
    }
}

// The arena: a light table, a translation, a tile, two solid texture
// columns behind a table, and a patch of two columns with posts
struct Fixture {
    Arena a;
    uint32_t cmap, trans, tile, table, patch;
    void build() {
        auto& m = a.bytes;
        m.assign(16, 0);
        cmap = m.size();
        for (uint32_t i = 0; i < kRasterLightRows * 256; i++) m.push_back(static_cast<uint8_t>(rnd()));
        trans = m.size();
        for (uint32_t i = 0; i < 256; i++) m.push_back(static_cast<uint8_t>(rnd()));
        tile = m.size();
        for (uint32_t i = 0; i < 4096; i++) m.push_back(static_cast<uint8_t>(rnd()));
        // a patch: 2 columns; column 0 two posts, column 1 one post
        patch = m.size();
        std::vector<uint8_t> p(8 + 8, 0);
        st16(p.data(), 2); st16(p.data() + 2, 60);
        st32(p.data() + 8, 16);
        p.insert(p.end(), {3, 10, 0});
        for (int i = 0; i < 10; i++) p.push_back(static_cast<uint8_t>(rnd()));
        p.push_back(0);
        p.insert(p.end(), {20, 7, 0});
        for (int i = 0; i < 7; i++) p.push_back(static_cast<uint8_t>(rnd()));
        p.push_back(0);
        p.push_back(0xff);
        st32(p.data() + 12, static_cast<uint32_t>(p.size()));
        p.insert(p.end(), {0, 40, 0});
        for (int i = 0; i < 40; i++) p.push_back(static_cast<uint8_t>(rnd()));
        p.push_back(0);
        p.push_back(0xff);
        m.insert(m.end(), p.begin(), p.end());
        // two columns of 128 texels behind a table, then 3 bytes of post header before each
        // so kind 2 can walk them as a single post of 128 texels
        table = m.size();
        for (int i = 0; i < 2; i++) m.insert(m.end(), {0, 0, 0, 0});
        for (int c = 0; c < 2; c++) {
            m.insert(m.end(), {0, 128, 0});
            st32(&m[table + 4 * c], static_cast<uint32_t>(m.size()));
            for (int i = 0; i < 128; i++) m.push_back(static_cast<uint8_t>(rnd()));
            m.push_back(0);
            m.push_back(0xff);
        }
        m.insert(m.end(), 256, 0);   // room past the last window
    }
};

static Col random_col(const Fixture& f, int kind) {
    Col c{};
    c.kind = static_cast<uint8_t>(kind);
    c.x = rnd() % kW;
    c.light = rnd() % kRasterLightRows;
    c.iscale = static_cast<int32_t>(rnd() % (3 << 16)) + 1000;
    c.texturemid = static_cast<int32_t>(rnd() % (200 << 16)) - (40 << 16);
    if (kind == 0) {
        c.src = f.table;
        c.index = rnd() % 2;
        c.a = rnd() % kH;
        c.b = static_cast<uint8_t>(c.a + rnd() % (kH - c.a));
    } else {
        c.src = kind == 1 ? f.patch : f.table;
        c.index = rnd() % 2;
        c.scale = static_cast<int32_t>(rnd() % (2 << 16)) + 2000;
        c.iscale = static_cast<int32_t>(0xffffffffu / static_cast<uint32_t>(c.scale));
        c.texturemid = static_cast<int32_t>(rnd() % (120 << 16)) - (20 << 16);
        c.a = static_cast<uint8_t>(1 + rnd() % 4);                       // ceiling clip -1 .. 2
        c.b = static_cast<uint8_t>(kH + 2 - rnd() % 4);                 // floor clip kH .. kH - 3
    }
    return c;
}

static void test_kernels_match_reference() {
    Fixture f;
    f.build();
    const uint8_t* cmap = f.a.bytes.data() + f.cmap;
    for (int round = 0; round < 40; round++) {
        const bool translated = round % 4 == 3;
        const uint8_t* trans = translated ? f.a.bytes.data() + f.trans : nullptr;
        const int32_t centery = static_cast<int32_t>(rnd() % kH);
        std::vector<Col> cols;
        std::vector<Span> spans;
        for (int i = 0; i < 24; i++) cols.push_back(random_col(f, i % 3));
        for (int i = 0; i < 12; i++) {
            Span s{};
            s.src = f.tile;
            s.xfrac = static_cast<int32_t>(rnd()); s.yfrac = static_cast<int32_t>(rnd());
            s.xstep = static_cast<int32_t>(rnd() % (4 << 16)) - (2 << 16); s.ystep = static_cast<int32_t>(rnd() % (4 << 16)) - (2 << 16);
            s.x1 = rnd() % kW; s.x2 = s.x1 + rnd() % (kW - s.x1);
            s.y = rnd() % kH; s.light = rnd() % kRasterLightRows;
            spans.push_back(s);
        }
        std::vector<uint8_t> want(kW * kH, 0xee), got(kW * kH, 0xee);
        for (const Col& c : cols) ref_column(want, f.a, c, cmap, trans, centery);
        for (const Span& s : spans) ref_span(want, f.a, s, cmap);
        std::vector<uint8_t> crec(cols.size() * kRasterColumnBytes), srec(spans.size() * kRasterSpanBytes);
        for (size_t i = 0; i < cols.size(); i++) pack(crec.data() + i * kRasterColumnBytes, cols[i]);
        for (size_t i = 0; i < spans.size(); i++) pack(srec.data() + i * kRasterSpanBytes, spans[i]);
        // band by band, bands of 7 rows, the last one short
        for (uint32_t y0 = 0; y0 < kH; y0 += 7) {
            const uint32_t rows = std::min<uint32_t>(7, kH - y0);
            std::vector<uint8_t> band(kW * rows, 0xee);
            uint32_t bad = 99;
            assert(raster_columns(band.data(), kW, static_cast<int32_t>(y0), rows, crec.data(), static_cast<uint32_t>(cols.size()), cmap, trans,
                                  centery, &Arena::resolve, &f.a, &bad) == K32_OK);
            assert(raster_spans(band.data(), kW, static_cast<int32_t>(y0), rows, srec.data(), static_cast<uint32_t>(spans.size()), cmap,
                                &Arena::resolve, &f.a, &bad) == K32_OK);
            std::memcpy(got.data() + y0 * kW, band.data(), band.size());
        }
        assert(got == want);
    }
    // Records the kernel refuses, and a source the resolver does not have
    {
        uint8_t rec[kRasterColumnBytes];
        uint8_t band[kW * 4];
        uint32_t bad = 0;
        Col c = random_col(f, 0);
        c.x = kW;
        pack(rec, c);
        assert(raster_columns(band, kW, 0, 4, rec, 1, cmap, nullptr, 20, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        c = random_col(f, 0);
        c.light = kRasterLightRows;
        pack(rec, c);
        assert(raster_columns(band, kW, 0, 4, rec, 1, cmap, nullptr, 20, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        c = random_col(f, 0);
        c.kind = 3;
        pack(rec, c);
        assert(raster_columns(band, kW, 0, 4, rec, 1, cmap, nullptr, 20, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        c = random_col(f, 1);
        c.index = 2;   // past the patch's width
        pack(rec, c);
        assert(raster_columns(band, kW, 0, 4, rec, 1, cmap, nullptr, 20, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        c = random_col(f, 0);
        c.src = static_cast<uint32_t>(f.a.bytes.size()) - 4;
        pack(rec, c);
        assert(raster_columns(band, kW, 0, 4, rec, 1, cmap, nullptr, 20, &Arena::resolve, &f.a, &bad) == K32_ERR_FAULT && bad == 0);
        uint8_t srec[kRasterSpanBytes];
        Span s{};
        s.src = f.tile; s.x1 = 5; s.x2 = 4; s.y = 1;
        pack(srec, s);
        assert(raster_spans(band, kW, 0, 4, srec, 1, cmap, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        s.x1 = 0; s.x2 = kW;
        pack(srec, s);
        assert(raster_spans(band, kW, 0, 4, srec, 1, cmap, &Arena::resolve, &f.a, &bad) == K32_ERR_INVAL);
        s.x2 = 3; s.src = static_cast<uint32_t>(f.a.bytes.size()) - 4000;
        pack(srec, s);
        assert(raster_spans(band, kW, 0, 4, srec, 1, cmap, &Arena::resolve, &f.a, &bad) == K32_ERR_FAULT);
    }
    std::printf("[PASS] test_kernels_match_reference\n");
}

// ---- a pod that imports the kernels and the blits ----
static void leb(std::vector<uint8_t>& b, uint32_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        b.push_back(byte | (v ? 0x80 : 0));
    } while (v);
}
static void name(std::vector<uint8_t>& b, const char* s) {
    leb(b, static_cast<uint32_t>(std::strlen(s)));
    b.insert(b.end(), s, s + std::strlen(s));
}
static void section(std::vector<uint8_t>& m, uint8_t id, const std::vector<uint8_t>& body) {
    m.push_back(id);
    leb(m, static_cast<uint32_t>(body.size()));
    m.insert(m.end(), body.begin(), body.end());
}

constexpr uint8_t kI32 = 0x7f;
static const char* const kImports[] = {"k32_raster_columns", "k32_raster_spans", "k32_display_blit", "k32_display_blit_pal"};
static const uint8_t kArity[] = {9, 7, 5, 6};
constexpr int kNum = 4;

// Each import has its own type; function i calls import i with its arguments
static std::vector<uint8_t> raster_module() {
    std::vector<uint8_t> m = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    std::vector<uint8_t> types = {kNum};
    for (int i = 0; i < kNum; i++) {
        types.push_back(0x60);
        leb(types, kArity[i]);
        for (int k = 0; k < kArity[i]; k++) types.push_back(kI32);
        types.push_back(0x01);
        types.push_back(kI32);
    }
    section(m, 1, types);
    std::vector<uint8_t> imports = {kNum};
    for (int i = 0; i < kNum; i++) {
        name(imports, "env");
        name(imports, kImports[i]);
        imports.push_back(0x00);
        imports.push_back(static_cast<uint8_t>(i));
    }
    section(m, 2, imports);
    std::vector<uint8_t> funcs = {kNum};
    for (int i = 0; i < kNum; i++) funcs.push_back(static_cast<uint8_t>(i));
    section(m, 3, funcs);
    section(m, 5, {0x01, 0x01, 0x02, 0x02});   // two pages
    std::vector<uint8_t> exports = {kNum + 1};
    const char* const fns[] = {"cols", "spans", "blit", "blitpal"};
    for (int i = 0; i < kNum; i++) {
        name(exports, fns[i]);
        exports.push_back(0x00);
        exports.push_back(static_cast<uint8_t>(kNum + i));
    }
    name(exports, "memory");
    exports.push_back(0x02);
    exports.push_back(0x00);
    section(m, 7, exports);
    std::vector<uint8_t> code = {kNum};
    for (int f = 0; f < kNum; f++) {
        std::vector<uint8_t> body = {0x00};
        for (uint8_t i = 0; i < kArity[f]; i++) { body.push_back(0x20); body.push_back(i); }
        body.push_back(0x10); body.push_back(static_cast<uint8_t>(f));
        body.push_back(0x0b);
        leb(code, static_cast<uint32_t>(body.size()));
        code.insert(code.end(), body.begin(), body.end());
    }
    section(m, 10, code);
    return m;
}

struct Pod {
    IM3Environment env{nullptr};
    IM3Runtime rt{nullptr};
    IM3Module mod{nullptr};
    IM3Function fn[kNum]{};
    std::vector<uint8_t> wasm;
    uint8_t* mem{nullptr};
    size_t mem_size{0};
    explicit Pod(uint64_t gas = 0) : wasm(raster_module()) {
        env = m3_NewEnvironment();
        rt = m3_NewRuntime(env, 16 * 1024, nullptr);
        if (gas) m3_SetResourceLimit(rt, c_m3Limit_GasUnits, gas);
        M3Result r = m3_ParseModule(env, &mod, wasm.data(), static_cast<uint32_t>(wasm.size()));
        if (!r) r = m3_LoadModule(rt, mod);
        assert(!r);
        link_app_host_functions(mod, "default/raster");
        const char* const fns[] = {"cols", "spans", "blit", "blitpal"};
        for (int i = 0; i < kNum && !r; i++) r = m3_FindFunction(&fn[i], rt, fns[i]);
        if (r) std::printf("module: %s\n", r);
        assert(!r);
        mem = m3_GetMemory(mod, &mem_size, 0);
        assert(mem && mem_size == 2 * 65536);
    }
    ~Pod() {
        unlink_app_host_functions(rt);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
    }
    template <typename... A>
    M3Result call(int i, int32_t* rc, A... args) {
        M3Result r = m3_CallV(fn[i], args...);
        if (!r && rc) m3_GetResultsV(fn[i], rc);
        return r;
    }
};

// The fixture laid into the pod's memory at kBase; the band at kBand, the records at kRec
constexpr uint32_t kBase = 0x100, kBand = 0x10000, kRec = 0x14000;

static void load(Pod& p, const Fixture& f) {
    assert(kBase + f.a.bytes.size() < kBand);
    std::memcpy(p.mem + kBase, f.a.bytes.data(), f.a.bytes.size());
    // the column table's offsets are the arena's: in the pod's memory they shift with it
    for (int c = 0; c < 2; c++) st32(p.mem + kBase + f.table + 4 * c, ld32(f.a.bytes.data() + f.table + 4 * c) + kBase);
}
// a record for the pod: the fixture's addresses shifted to kBase
static Col shifted(Col c) { c.src += kBase; return c; }
static Span shifted(Span s) { s.src += kBase; return s; }

static void test_imports() {
    Fixture f;
    f.build();
    Pod p;
    load(p, f);
    const uint8_t* cmap = f.a.bytes.data() + f.cmap;
    const int32_t centery = 20;
    std::vector<Col> cols;
    for (int i = 0; i < 9; i++) cols.push_back(random_col(f, i % 3));
    std::vector<Span> spans;
    for (int i = 0; i < 5; i++) {
        Span s{};
        s.src = f.tile; s.xfrac = static_cast<int32_t>(rnd()); s.yfrac = static_cast<int32_t>(rnd());
        s.xstep = 3000; s.ystep = -5000; s.x1 = i * 3; s.x2 = s.x1 + 20; s.y = i * 7; s.light = i;
        spans.push_back(s);
    }
    std::vector<uint8_t> want(kW * kH, 0);
    for (const Col& c : cols) ref_column(want, f.a, c, cmap, nullptr, centery);
    for (const Span& s : spans) ref_span(want, f.a, s, cmap);
    for (size_t i = 0; i < cols.size(); i++) pack(p.mem + kRec + i * kRasterColumnBytes, shifted(cols[i]));
    const uint32_t kSRec = kRec + 0x1000;
    for (size_t i = 0; i < spans.size(); i++) pack(p.mem + kSRec + i * kRasterSpanBytes, shifted(spans[i]));
    std::memset(p.mem + kBand, 0, kW * kH);
    int32_t rc = 1;
    M3Result r0 = p.call(0, &rc, kBand, kW, 0, kH, kRec, static_cast<uint32_t>(cols.size()), kBase + f.cmap, 0u, static_cast<uint32_t>(centery));
    if (r0 || rc != K32_OK) std::printf("columns: %s rc %d\n", r0 ? r0 : "ok", rc);
    assert(!r0 && rc == K32_OK);
    assert(!p.call(1, &rc, kBand, kW, 0, kH, kSRec, static_cast<uint32_t>(spans.size()), kBase + f.cmap) && rc == K32_OK);
    assert(std::memcmp(p.mem + kBand, want.data(), want.size()) == 0);
    // Half the rows as a band: the rest untouched (the band's own fill is the reference's)
    std::vector<uint8_t> want7(kW * kH, 7);
    for (const Col& c : cols) ref_column(want7, f.a, c, cmap, nullptr, centery);
    for (const Span& s : spans) ref_span(want7, f.a, s, cmap);
    want = want7;
    std::memset(p.mem + kBand, 7, kW * kH);
    assert(!p.call(0, &rc, kBand, kW, 20, kH - 20, kRec, static_cast<uint32_t>(cols.size()), kBase + f.cmap, 0u, static_cast<uint32_t>(centery)) && rc == K32_OK);
    assert(!p.call(1, &rc, kBand, kW, 20, kH - 20, kSRec, static_cast<uint32_t>(spans.size()), kBase + f.cmap) && rc == K32_OK);
    for (uint32_t i = 0; i < (kH - 20) * kW; i++)
        if (p.mem[kBand + i] != want[20 * kW + i]) { std::printf("band mismatch at row %u col %u: %u want %u\n", i / kW, i % kW, p.mem[kBand + i], want[20 * kW + i]); break; }
    assert(std::memcmp(p.mem + kBand, want.data() + 20 * kW, (kH - 20) * kW) == 0);
    for (uint32_t i = (kH - 20) * kW; i < kW * kH; i++) assert(p.mem[kBand + i] == 7);
    // A record the kernel refuses
    Col bad = shifted(cols[0]);
    bad.x = kW;
    pack(p.mem + kRec, bad);
    assert(!p.call(0, &rc, kBand, kW, 0, kH, kRec, 1u, kBase + f.cmap, 0u, static_cast<uint32_t>(centery)) && rc == K32_ERR_INVAL);
    pack(p.mem + kRec, shifted(cols[0]));
    assert(!p.call(0, &rc, kBand, 0u, 0, kH, kRec, 1u, kBase + f.cmap, 0u, static_cast<uint32_t>(centery)) && rc == K32_ERR_INVAL);
    // Operands outside the memory trap, named
    const uint32_t end = static_cast<uint32_t>(p.mem_size);
    auto trap = [&](M3Result r, const char* operand, const char* fn) {
        assert(r);
        const std::string want_msg = std::string("[trap] out of bounds memory access: ") + operand + " of " + fn;
        if (want_msg != r) std::printf("trap: %s\n", r);
        assert(want_msg == r);
        assert(describe_trap(r, TrapContext{}).exit_code == 139);
    };
    trap(p.call(0, nullptr, end - kW * kH + 1, kW, 0, kH, kRec, 1u, kBase + f.cmap, 0u, 20u), "dst", "k32_raster_columns");
    trap(p.call(0, nullptr, kBand, kW, 0, kH, end - 23, 1u, kBase + f.cmap, 0u, 20u), "records", "k32_raster_columns");
    trap(p.call(0, nullptr, kBand, kW, 0, kH, kRec, 1u, end - 34 * 256 + 1, 0u, 20u), "colormap", "k32_raster_columns");
    trap(p.call(0, nullptr, kBand, kW, 0, kH, kRec, 1u, kBase + f.cmap, end - 255, 20u), "translation", "k32_raster_columns");
    bad = shifted(cols[0]);
    bad.src = end - 4;
    pack(p.mem + kRec, bad);
    trap(p.call(0, nullptr, kBand, kW, 0, kH, kRec, 1u, kBase + f.cmap, 0u, 20u), "src", "k32_raster_columns");
    trap(p.call(1, nullptr, end - kW * kH + 1, kW, 0, kH, kSRec, 1u, kBase + f.cmap), "dst", "k32_raster_spans");
    trap(p.call(1, nullptr, kBand, kW, 0, kH, end - 27, 1u, kBase + f.cmap), "records", "k32_raster_spans");
    trap(p.call(1, nullptr, kBand, kW, 0, kH, kSRec, 1u, end - 34 * 256 + 1), "colormap", "k32_raster_spans");
    Span sbad = shifted(spans[0]);
    sbad.src = end - 4095;
    pack(p.mem + kSRec, sbad);
    trap(p.call(1, nullptr, kBand, kW, 0, kH, kSRec, 1u, kBase + f.cmap), "src", "k32_raster_spans");
    // Gas: a metered pod pays 64 units a record on top of its own instructions;
    // a budget below the charge traps out of gas
    {
        const uint64_t charge = linalg_gas(kRasterUnitsPerRecord * 3);
        Pod metered(4 * charge);
        load(metered, f);
        for (size_t i = 0; i < 3; i++) pack(metered.mem + kRec + i * kRasterColumnBytes, shifted(cols[i]));
        const uint64_t before = m3_GetResourceUsage(metered.rt, c_m3Limit_GasUnits);
        M3Result r = metered.call(0, &rc, kBand, kW, 0, kH, kRec, 3u, kBase + f.cmap, 0u, 20u);
        if (r) std::printf("metered: %s\n", r);
        assert(!r && rc == K32_OK);
        const uint64_t used = m3_GetResourceUsage(metered.rt, c_m3Limit_GasUnits) - before;
        assert(used >= charge && used < charge + 4000);   // the rest is the module's own instructions
        Pod starved(charge - 1);
        load(starved, f);
        for (size_t i = 0; i < 3; i++) pack(starved.mem + kRec + i * kRasterColumnBytes, shifted(cols[i]));
        r = starved.call(0, &rc, kBand, kW, 0, kH, kRec, 3u, kBase + f.cmap, 0u, 20u);
        assert(r && std::string(r) == m3Err_trapOutOfGas);
    }
    std::printf("[PASS] test_imports\n");
}

// A region whose bytes are not in the address space (a file on a board's card)
class UnmappedRegion : public kube32::storage::BlobRegion {
public:
    explicit UnmappedRegion(size_t size) : ram_(kube32::storage::create_ram_blob_region(size)) {}
    size_t size() const override { return ram_->size(); }
    bool read(size_t o, void* d, size_t n) override { return ram_->read(o, d, n); }
    bool write(size_t o, const void* s, size_t n) override { return ram_->write(o, s, n); }
    bool erase(size_t o, size_t n) override { return ram_->erase(o, n); }
    const uint8_t* data() const override { return nullptr; }
    std::string name() const override { return "unmapped"; }

private:
    std::unique_ptr<kube32::storage::BlobRegion> ram_;
};
static void store_blob(kube32::storage::BlobStore& store, const char* name, const std::vector<uint8_t>& bytes) {
    int32_t err = 0;
    std::string why;
    auto w = store.create(name, bytes.size(), err, why);
    assert(w && w->write(bytes.data(), bytes.size(), why) == K32_OK && w->commit(why) == K32_OK);
}
static uint32_t blob_addr(int32_t handle, uint32_t offset) {
    return K32_BLOB_ADDR_FLAG | (static_cast<uint32_t>(handle) << K32_BLOB_HANDLE_SHIFT) | offset;
}

// Texels, tables and the light table read in place from a blob give the
// bytes they give from memory; outside every open blob traps; not mapped
// answers K32_ERR_NOTSUP
static void test_blob_sources() {
    using namespace kube32;
    const std::string pod = "default/raster";
    Fixture f;
    f.build();
    storage::FlashBlobStore flash(storage::create_ram_blob_region(512 * 1024), false);
    storage::FlashBlobStore unmapped(std::make_unique<UnmappedRegion>(512 * 1024), false);
    store_blob(flash, "pack", f.a.bytes);
    store_blob(unmapped, "pack", f.a.bytes);
    Pod p;
    load(p, f);
    const uint8_t* cmap = f.a.bytes.data() + f.cmap;
    std::vector<Col> cols;
    for (int i = 0; i < 9; i++) cols.push_back(random_col(f, i % 3));
    Span s{};
    s.src = f.tile; s.xfrac = 12345678; s.yfrac = -2345678; s.xstep = 3000; s.ystep = -5000; s.x1 = 2; s.x2 = 50; s.y = 9; s.light = 3;
    std::vector<uint8_t> want(kW * kH, 0);
    for (const Col& c : cols) ref_column(want, f.a, c, cmap, nullptr, 20);
    ref_span(want, f.a, s, cmap);
    storage::set_node_blob_stores(&flash, nullptr);
    PodBlobs::instance().set_allowed(pod, {"pack"});
    const int32_t h = PodBlobs::instance().open(pod, "pack");
    assert(h == 0 && PodBlobs::instance().mapped(pod, h) == 1);
    for (size_t i = 0; i < cols.size(); i++) {
        Col c = cols[i];
        c.src = blob_addr(h, c.src);
        pack(p.mem + kRec + i * kRasterColumnBytes, c);
    }
    Span sb = s;
    sb.src = blob_addr(h, s.src);
    const uint32_t kSRec = kRec + 0x1000;
    pack(p.mem + kSRec, sb);
    std::memset(p.mem + kBand, 0, kW * kH);
    int32_t rc = 1;
    assert(!p.call(0, &rc, kBand, kW, 0, kH, kRec, 9u, blob_addr(h, f.cmap), 0u, 20u) && rc == K32_OK);
    assert(!p.call(1, &rc, kBand, kW, 0, kH, kSRec, 1u, blob_addr(h, f.cmap)) && rc == K32_OK);
    assert(std::memcmp(p.mem + kBand, want.data(), want.size()) == 0);
    // A table whose entry points past the blob, a handle not open
    Col c = cols[0];
    c.src = blob_addr(h, static_cast<uint32_t>(f.a.bytes.size()) - 4);
    pack(p.mem + kRec, c);
    M3Result r = p.call(0, nullptr, kBand, kW, 0, kH, kRec, 1u, blob_addr(h, f.cmap), 0u, 20u);
    assert(r && std::string(r) == "[trap] out of bounds memory access: src of k32_raster_columns (a blob address outside every open blob)");
    sb.src = blob_addr(1, 0);
    pack(p.mem + kSRec, sb);
    r = p.call(1, nullptr, kBand, kW, 0, kH, kSRec, 1u, blob_addr(h, f.cmap));
    assert(r && std::string(r) == "[trap] out of bounds memory access: src of k32_raster_spans (a blob address outside every open blob)");
    PodBlobs::instance().forget_pod(pod);
    storage::set_node_blob_stores(&unmapped, nullptr);
    PodBlobs::instance().set_allowed(pod, {"pack"});
    assert(PodBlobs::instance().open(pod, "pack") == 0 && PodBlobs::instance().mapped(pod, 0) == 0);
    pack(p.mem + kRec, cols[0]);
    c = cols[0];
    c.src = blob_addr(0, c.src);
    pack(p.mem + kRec, c);
    assert(!p.call(0, &rc, kBand, kW, 0, kH, kRec, 1u, kBase + f.cmap, 0u, 20u) && rc == K32_ERR_NOTSUP);
    PodBlobs::instance().forget_pod(pod);
    storage::set_node_blob_stores(nullptr, nullptr);
    std::printf("[PASS] test_blob_sources\n");
}

// The blits reach the platform hook with the pod's pixels, need the display
// grant, and refuse a size of zero or pixels outside the memory
struct Captured {
    int32_t x, y, w, h;
    std::vector<uint16_t> px;
};
static std::vector<Captured> s_blits;

static void test_blits() {
    using namespace kube32::hal;
    AppPlatformHooks hooks;
    hooks.display_blit = [](int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) -> int32_t {
        Captured c{x, y, w, h, std::vector<uint16_t>(pixels, pixels + static_cast<size_t>(w) * h)};
        s_blits.push_back(std::move(c));
        return 0;
    };
    set_app_platform_hooks(hooks);
    Pod p;
    int32_t rc = 1;
    // No grant: K32_ERR_PERM
    CapabilityFirewall::instance().reset();
    assert(!p.call(2, &rc, 10, 20, 4, 3, kBand) && rc == K32_ERR_PERM);
    DeviceGrant grant;
    grant.capabilities_mask = KUBE32_CAP_DISPLAY;
    std::string err;
    assert(CapabilityFirewall::instance().grant_capabilities("default/raster", grant, err));
    // 4 x 3 pixels at an odd address
    std::vector<uint16_t> px(12);
    for (auto& v : px) v = static_cast<uint16_t>(rnd());
    std::memcpy(p.mem + kBand + 1, px.data(), px.size() * 2);
    s_blits.clear();
    assert(!p.call(2, &rc, 10, 20, 4, 3, kBand + 1) && rc == K32_OK);
    assert(s_blits.size() == 1 && s_blits[0].x == 10 && s_blits[0].y == 20 && s_blits[0].w == 4 && s_blits[0].h == 3 && s_blits[0].px == px);
    // A paletted 5 x 19 image goes through the palette in bands of 8 rows
    uint16_t pal[256];
    for (auto& v : pal) v = static_cast<uint16_t>(rnd());
    std::vector<uint8_t> idx(5 * 19);
    for (auto& v : idx) v = static_cast<uint8_t>(rnd());
    std::memcpy(p.mem + 0x2000, idx.data(), idx.size());
    std::memcpy(p.mem + 0x3001, pal, sizeof pal);
    s_blits.clear();
    assert(!p.call(3, &rc, -2, 7, 5, 19, 0x2000, 0x3001) && rc == K32_OK);
    assert(s_blits.size() == 3);
    std::vector<uint16_t> all;
    int32_t y = 7;
    for (const Captured& c : s_blits) {
        assert(c.x == -2 && c.y == y && c.w == 5);
        y += c.h;
        all.insert(all.end(), c.px.begin(), c.px.end());
    }
    assert(y == 7 + 19 && all.size() == idx.size());
    for (size_t i = 0; i < idx.size(); i++) assert(all[i] == pal[idx[i]]);
    // Sizes the import refuses, pixels outside the memory
    assert(!p.call(2, &rc, 0, 0, 0, 3, kBand) && rc == K32_ERR_INVAL);
    assert(!p.call(2, &rc, 0, 0, 4, 1025, kBand) && rc == K32_ERR_INVAL);
    assert(!p.call(2, &rc, 0, 0, 4, 3, static_cast<uint32_t>(p.mem_size) - 23) && rc == K32_ERR_FAULT);
    assert(!p.call(3, &rc, 0, 0, 4, 3, static_cast<uint32_t>(p.mem_size) - 11, 0x3001) && rc == K32_ERR_FAULT);
    assert(!p.call(3, &rc, 0, 0, 4, 3, 0x2000, static_cast<uint32_t>(p.mem_size) - 511) && rc == K32_ERR_FAULT);
    CapabilityFirewall::instance().reset();
    set_app_platform_hooks(AppPlatformHooks{});
    std::printf("[PASS] test_blits\n");
}

int main() {
    test_kernels_match_reference();
    test_imports();
    test_blob_sources();
    test_blits();
    std::printf("All pod raster tests passed\n");
    return 0;
}
