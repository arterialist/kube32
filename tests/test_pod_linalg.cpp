// The pods' linear algebra imports (k32_matvec_*, k32_matmul_*,
// k32_vecmat_f32_i8, k32_softmax_f32 and the element-wise ones): the native
// kernels against plain reference loops and the reference's bits, and the
// imports as a pod calls them: results, strides, operands outside the linear
// memory, a trapped container, and the gas they charge.
#include "runtime/pod_linalg.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/container_lifecycle.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_blobs.hpp"
#include "storage/blob_store.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kube32::runtime;

// ---- reference ---------------------------------------------------------------
static uint32_t s_seed = 12345;
static uint32_t rnd() { return s_seed = s_seed * 1664525u + 1013904223u; }

static int32_t weight(const std::vector<int8_t>& w, size_t stride, int bits, uint32_t r, uint32_t c) {
    if (bits == 8) return w[r * stride + c];
    const int32_t b = static_cast<uint8_t>(w[r * stride + c / 2]);
    const int32_t nib = (c & 1) ? (b >> 4) : (b & 15);
    return nib >= 8 ? nib - 16 : nib;
}

static std::vector<float> reference(const std::vector<int8_t>& w, size_t stride, int bits, const std::vector<float>& ws,
                                    const std::vector<int8_t>& x, float xs, uint32_t rows, uint32_t cols) {
    std::vector<float> out(rows);
    for (uint32_t r = 0; r < rows; r++) {
        int64_t acc = 0;
        for (uint32_t c = 0; c < cols; c++) acc += weight(w, stride, bits, r, c) * x[c];
        const float scale = ws[r] * xs;
        out[r] = static_cast<float>(static_cast<int32_t>(acc)) * scale;
    }
    return out;
}

struct Case {
    int bits;
    uint32_t rows, cols;
    size_t stride;   // bytes from one row to the next
};

static void fill(Case k, std::vector<int8_t>& w, std::vector<float>& ws, std::vector<int8_t>& x, float& xs) {
    w.resize(k.rows * k.stride + 1);
    for (auto& b : w) b = static_cast<int8_t>(rnd() >> 24);
    ws.resize(k.rows);
    for (auto& s : ws) s = static_cast<float>(rnd() >> 8) / 16777216.0f * 0.05f;
    x.resize(k.cols);
    for (auto& v : x) v = static_cast<int8_t>(rnd() >> 24);
    if (k.cols) x[0] = -128;   // the extremes: -128 x -128, -8
    if (k.cols) w[0] = static_cast<int8_t>(0x88);
    xs = 0.0123f;
}

static void test_kernels_match_reference() {
    const Case cases[] = {{8, 64, 64, 64},  {8, 172, 64, 64}, {8, 64, 172, 172}, {8, 7, 13, 13}, {8, 5, 3, 9},  {8, 1, 1, 1},
                          {4, 64, 64, 32},  {4, 64, 172, 86}, {4, 172, 64, 32},  {4, 9, 13, 7},  {4, 3, 5, 11}, {4, 2, 1, 1},
                          {8, 3, 0, 0},     {4, 3, 0, 0}};
    for (const Case& k : cases) {
        std::vector<int8_t> w, x;
        std::vector<float> ws;
        float xs;
        fill(k, w, ws, x, xs);
        const std::vector<float> want = reference(w, k.stride, k.bits, ws, x, xs, k.rows, k.cols);
        std::vector<float> got(k.rows + 1, -1.0f);
        // Float operands one byte off their alignment: the guest may pass any address
        std::vector<uint8_t> out_bytes(4 * k.rows + 1), ws_bytes(4 * k.rows + 1);
        std::memcpy(ws_bytes.data() + 1, ws.data(), 4 * k.rows);
        (k.bits == 8 ? matvec_i8 : matvec_i4)(out_bytes.data() + 1, w.data(), k.stride, ws_bytes.data() + 1, x.data(), xs, k.rows, k.cols);
        std::memcpy(got.data(), out_bytes.data() + 1, 4 * k.rows);
        assert(std::memcmp(got.data(), want.data(), 4 * k.rows) == 0);
        assert(got[k.rows] == -1.0f);
    }
    // The int32 sum wraps: 140,000 columns of 127 x 127 pass 2^31
    const uint32_t n = 140000;
    std::vector<int8_t> big(n, 127);
    const float one = 1.0f;
    float out = 0;
    matvec_i8(reinterpret_cast<uint8_t*>(&out), big.data(), n, reinterpret_cast<const uint8_t*>(&one), big.data(), 1.0f, 1, n);
    const int32_t wrapped = static_cast<int32_t>(static_cast<uint32_t>(uint64_t{n} * 127 * 127));
    assert(out == static_cast<float>(wrapped));
    std::printf("[PASS] test_kernels_match_reference\n");
}


// n vectors at once give the bits of n matvec calls, at every group size
// (kMatmulGroup) and remainder, with float operands off their alignment
static void test_matmul_kernels() {
    const Case cases[] = {{8, 64, 64, 64}, {8, 7, 13, 20}, {8, 1, 1, 1}, {4, 64, 172, 86}, {4, 9, 13, 9}, {4, 3, 1, 1}, {8, 3, 0, 0}};
    for (const Case& k : cases) {
        for (uint32_t n : {1u, 2u, 3u, 4u, 5u, 7u, 9u}) {
            std::vector<int8_t> w, x0;
            std::vector<float> ws;
            float xs0;
            fill(k, w, ws, x0, xs0);
            std::vector<int8_t> x(static_cast<size_t>(n) * k.cols + 1);
            for (auto& v : x) v = static_cast<int8_t>(rnd() >> 24);
            std::vector<uint8_t> xs_bytes(4 * n + 1), ws_bytes(4 * k.rows + 1), out_bytes(4 * n * k.rows + 2, 0xa5);
            std::vector<float> xs(n);
            for (uint32_t p = 0; p < n; p++) xs[p] = static_cast<float>(rnd() >> 8) / 16777216.0f * 0.1f;
            std::memcpy(xs_bytes.data() + 1, xs.data(), 4 * n);
            std::memcpy(ws_bytes.data() + 1, ws.data(), 4 * k.rows);
            (k.bits == 8 ? matmul_i8 : matmul_i4)(out_bytes.data() + 1, w.data(), k.stride, ws_bytes.data() + 1, x.data(), xs_bytes.data() + 1,
                                                  k.rows, k.cols, n);
            for (uint32_t p = 0; p < n; p++) {
                const std::vector<int8_t> xp(x.begin() + p * k.cols, x.begin() + (p + 1) * k.cols);
                const std::vector<float> want = reference(w, k.stride, k.bits, ws, xp, xs[p], k.rows, k.cols);
                std::vector<uint8_t> one(4 * k.rows + 1);
                (k.bits == 8 ? matvec_i8 : matvec_i4)(one.data() + 1, w.data(), k.stride, ws_bytes.data() + 1, xp.data(), xs[p], k.rows, k.cols);
                assert(std::memcmp(out_bytes.data() + 1 + 4 * p * k.rows, want.data(), 4 * k.rows) == 0);
                assert(std::memcmp(out_bytes.data() + 1 + 4 * p * k.rows, one.data() + 1, 4 * k.rows) == 0);
            }
            assert(out_bytes[0] == 0xa5 && out_bytes[4 * n * k.rows + 1] == 0xa5);
        }
    }
    std::printf("[PASS] test_matmul_kernels\n");
}

// ---- float kernels -----------------------------------------------------------------
// The test is built with -ffp-contract=off, as the kernels are: each line
// below is the operation order the ABI defines
static std::vector<float> ref_matvec_f32(const std::vector<int8_t>& w, size_t stride, const std::vector<float>& ws, size_t sstride,
                                         const std::vector<float>& x, float alpha, uint32_t rows, uint32_t cols) {
    std::vector<float> out(rows);
    for (uint32_t r = 0; r < rows; r++) {
        float d = 0.0f;
        for (uint32_t c = 0; c < cols; c++) d = d + x[c] * static_cast<float>(w[r * stride + c]);
        d = d * ws[r * sstride];
        out[r] = d * alpha;
    }
    return out;
}

static std::vector<float> ref_vecmat_f32(const std::vector<int8_t>& w, size_t stride, const std::vector<float>& ws, size_t sstride,
                                         const std::vector<float>& x, uint32_t rows, uint32_t cols) {
    std::vector<float> out(cols, 0.0f);
    for (uint32_t r = 0; r < rows; r++) {
        const float a = x[r] * ws[r * sstride];
        for (uint32_t c = 0; c < cols; c++) out[c] = out[c] + a * static_cast<float>(w[r * stride + c]);
    }
    return out;
}

static float bits_f32(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

static float rnd_f32() { return static_cast<float>(static_cast<int32_t>(rnd() >> 8) - (1 << 23)) / 4194304.0f; }

static void test_float_kernels() {
    // k32_expf and the native softmax give the reference's bits (tools/llm.py
    // of apps/llm-pipeline computes the same operations with numpy). A fused
    // multiply-add changes those of -19.890625, -9.390625, -1.15625 and 7.953125.
    const float xs[] = {-100.0f, -87.5f, -20.25f, -3.0f, -0.6931f, -1e-3f, 0.0f, 1e-3f, 0.5f, 1.0f, 2.718f, 10.0f, 42.5f, 88.0f, 95.0f,
                        -19.890625f, -9.390625f, -1.15625f, 7.953125f};
    const uint32_t exp_bits[] = {0x0,        0x0,        0x30dc9ef1, 0x3d4bed86, 0x3f00018c, 0x3f7fbe7f, 0x3f800000,
                                 0x3f8020c9, 0x3fd3094c, 0x402df854, 0x4172665d, 0x46ac14ee, 0x5e1f2ec2, 0x7ef882b7,
                                 0x7ef882b7, 0x311e032c, 0x38af1e76, 0x3ea11ba2, 0x4531c732};
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        const float e = k32_expf(xs[i]);
        uint32_t b;
        std::memcpy(&b, &e, 4);
        assert(b == exp_bits[i]);
    }
    const float sm_in[] = {-1.15625f, -19.890625f, 0.0f, -9.390625f, -3.0f, 0.0f, -0.5f};
    const uint32_t sm_bits[] = {0x3dd8e711, 0x3054bc2a, 0x3eac542b, 0x37ebc415, 0x3c8946a2, 0x3eac542b, 0x3e510b9e};
    uint8_t sm[1 + sizeof sm_in];
    std::memcpy(sm + 1, sm_in, sizeof sm_in);
    softmax_f32(sm + 1, 7);
    for (int i = 0; i < 7; i++) assert(std::memcmp(sm + 1 + 4 * i, &sm_bits[i], 4) == 0);

    // Scores over a cache whose rows interleave heads, and sums wider than a block
    struct FCase {
        uint32_t rows, cols;
        size_t stride, sstride;
    };
    for (FCase k : {FCase{37, 8, 32, 4}, FCase{1, 8, 8, 1}, FCase{9, 150, 151, 2}, FCase{5, 64, 64, 1}, FCase{4, 0, 3, 1}}) {
        std::vector<int8_t> w(k.rows * k.stride + 1);
        for (auto& b : w) b = static_cast<int8_t>(rnd() >> 24);
        std::vector<float> ws(k.rows * k.sstride + 1), xr(k.cols), xc(k.rows);
        for (auto& f : ws) f = rnd_f32() * 0.01f;
        for (auto& f : xr) f = rnd_f32();
        for (auto& f : xc) f = rnd_f32();
        const float alpha = bits_f32(0x3eb504f3);   // 1 / sqrt(8)
        std::vector<uint8_t> out(4 * (k.rows + k.cols) + 1), wsb(4 * ws.size() + 1), xb(4 * (k.rows + k.cols) + 1);
        std::memcpy(wsb.data() + 1, ws.data(), 4 * ws.size());
        std::memcpy(xb.data() + 1, xr.data(), 4 * k.cols);
        matvec_f32_i8(out.data() + 1, w.data(), k.stride, wsb.data() + 1, 4 * k.sstride, xb.data() + 1, alpha, k.rows, k.cols);
        const std::vector<float> want = ref_matvec_f32(w, k.stride, ws, k.sstride, xr, alpha, k.rows, k.cols);
        assert(std::memcmp(out.data() + 1, want.data(), 4 * k.rows) == 0);
        std::memcpy(xb.data() + 1, xc.data(), 4 * k.rows);
        vecmat_f32_i8(out.data() + 1, w.data(), k.stride, wsb.data() + 1, 4 * k.sstride, xb.data() + 1, k.rows, k.cols);
        const std::vector<float> want2 = ref_vecmat_f32(w, k.stride, ws, k.sstride, xc, k.rows, k.cols);
        assert(std::memcmp(out.data() + 1, want2.data(), 4 * k.cols) == 0);
    }
    std::printf("[PASS] test_float_kernels\n");
}

// ---- a module that calls the imports --------------------------------------------
static void leb(std::vector<uint8_t>& b, uint32_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        b.push_back(byte | (v ? 0x80 : 0));
    } while (v);
}
static void sleb(std::vector<uint8_t>& b, int32_t v) {
    for (;;) {
        const uint8_t byte = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
            b.push_back(byte);
            return;
        }
        b.push_back(byte | 0x80);
    }
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

// The imports, with functions "mv8", "mv4", "mvf", "vm", "sm", "mm8", "mm4",
// "rms", "qnt", "rope", "swi", "add" and "amax" that pass their arguments on,
// "oob" that calls k32_matvec_i8 with w past the end of memory, and two pages
// of memory
constexpr uint8_t kI32 = 0x7f, kF32 = 0x7d;
static const std::vector<std::vector<uint8_t>> kParams = {{kI32, kI32, kI32, kI32, kF32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32, kI32, kF32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32, kI32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32},
                                                          {},
                                                          {kI32, kI32, kI32, kI32, kI32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32, kI32, kI32, kF32},
                                                          {kI32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32}};
constexpr uint8_t kNumImports = 13;
static const char* const kImports[] = {"k32_matvec_i8",   "k32_matvec_i4",   "k32_matvec_f32_i8", "k32_vecmat_f32_i8", "k32_softmax_f32",
                                       "k32_matmul_i8",   "k32_matmul_i4",   "k32_rmsnorm_f32",   "k32_quantize_i8",   "k32_rope_f32",
                                       "k32_swiglu_f32",  "k32_add_f32",     "k32_argmax_f32"};
static const uint8_t kImportType[] = {0, 0, 1, 2, 3, 5, 5, 6, 7, 7, 8, 8, 3};
// The module's functions after the imports, their types, and the import each calls (oob: k32_matvec_i8)
constexpr uint8_t kNumFns = 14;
static const char* const kFns[] = {"mv8", "mv4", "mvf", "vm", "sm", "oob", "mm8", "mm4", "rms", "qnt", "rope", "swi", "add", "amax"};
static const uint8_t kFnType[] = {0, 0, 1, 2, 3, 4, 5, 5, 6, 7, 7, 8, 8, 3};
static const uint8_t kFnCallee[] = {0, 1, 2, 3, 4, 0, 5, 6, 7, 8, 9, 10, 11, 12};

static std::vector<uint8_t> linalg_module() {
    std::vector<uint8_t> m = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    std::vector<uint8_t> types = {static_cast<uint8_t>(kParams.size())};
    for (const auto& p : kParams) {
        types.push_back(0x60);
        leb(types, static_cast<uint32_t>(p.size()));
        types.insert(types.end(), p.begin(), p.end());
        types.push_back(0x01);
        types.push_back(kI32);
    }
    section(m, 1, types);
    std::vector<uint8_t> imports = {kNumImports};
    for (int i = 0; i < kNumImports; i++) {
        name(imports, "env");
        name(imports, kImports[i]);
        imports.push_back(0x00);
        imports.push_back(kImportType[i]);
    }
    section(m, 2, imports);
    std::vector<uint8_t> funcs = {kNumFns};
    funcs.insert(funcs.end(), kFnType, kFnType + kNumFns);
    section(m, 3, funcs);
    section(m, 5, {0x01, 0x01, 0x02, 0x02});
    std::vector<uint8_t> exports = {kNumFns + 1};
    for (uint8_t i = 0; i < kNumFns; i++) {
        name(exports, kFns[i]);
        exports.push_back(0x00);
        exports.push_back(kNumImports + i);
    }
    name(exports, "memory");
    exports.push_back(0x02);
    exports.push_back(0x00);
    section(m, 7, exports);
    std::vector<uint8_t> code = {kNumFns};
    for (uint8_t f = 0; f < kNumFns; f++) {
        std::vector<uint8_t> body = {0x00};
        if (std::strcmp(kFns[f], "oob") != 0) {
            for (uint8_t i = 0; i < kParams[kFnType[f]].size(); i++) { body.push_back(0x20); body.push_back(i); }
            body.push_back(0x10); body.push_back(kFnCallee[f]);
        } else {
            for (int32_t v : {0x100, 0x1FFF0, 0x9001, 0xA000}) { body.push_back(0x41); sleb(body, v); }
            body.insert(body.end(), {0x43, 0x00, 0x00, 0x80, 0x3f});   // f32.const 1.0
            for (int32_t v : {16, 32, 0}) { body.push_back(0x41); sleb(body, v); }
            body.push_back(0x10); body.push_back(0x00);
        }
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
    IM3Function fn[13]{};   // mv8, mv4, mvf, vm, sm, mm8, mm4, rms, qnt, rope, swi, add, amax
    std::vector<uint8_t> wasm;
    uint8_t* mem{nullptr};
    size_t mem_size{0};

    explicit Pod(uint64_t gas = 0) : wasm(linalg_module()) {
        env = m3_NewEnvironment();
        rt = m3_NewRuntime(env, 16 * 1024, nullptr);
        // A limit set before the functions compile meters them, as for a metered pod
        if (gas) m3_SetResourceLimit(rt, c_m3Limit_GasUnits, gas);
        M3Result r = m3_ParseModule(env, &mod, wasm.data(), static_cast<uint32_t>(wasm.size()));
        if (!r) r = m3_LoadModule(rt, mod);
        assert(!r);
        link_app_host_functions(mod, "default/linalg");
        const char* const names[] = {"mv8", "mv4", "mvf", "vm", "sm", "mm8", "mm4", "rms", "qnt", "rope", "swi", "add", "amax"};
        for (int i = 0; i < 13 && !r; i++) r = m3_FindFunction(&fn[i], rt, names[i]);
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
    M3Result call(int bits, uint32_t out, uint32_t w, uint32_t ws, uint32_t x, float xs, uint32_t rows, uint32_t cols, uint32_t stride,
                  int32_t* rc = nullptr) {
        IM3Function f = bits == 8 ? fn[0] : fn[1];
        M3Result r = m3_CallV(f, out, w, ws, x, static_cast<double>(xs), rows, cols, stride);
        if (!r && rc) m3_GetResultsV(f, rc);
        return r;
    }
    template <typename... A>
    M3Result call_fn(int i, int32_t* rc, A... args) {
        M3Result r = m3_CallV(fn[i], args...);
        if (!r && rc) m3_GetResultsV(fn[i], rc);
        return r;
    }
};

// Operands at fixed places in the pod's memory
constexpr uint32_t kOut = 0x100, kW = 0x1000, kWs = 0x9001, kX = 0xA000;

static std::vector<float> run_case(Pod& p, Case k, std::vector<float>* want) {
    std::vector<int8_t> w, x;
    std::vector<float> ws;
    float xs;
    fill(k, w, ws, x, xs);
    std::memcpy(p.mem + kW, w.data(), w.size());
    std::memcpy(p.mem + kWs, ws.data(), 4 * ws.size());
    std::memcpy(p.mem + kX, x.data(), x.size());
    int32_t rc = 1;
    M3Result r = p.call(k.bits, kOut, kW, kWs, kX, xs, k.rows, k.cols, static_cast<uint32_t>(k.stride), &rc);
    assert(!r && rc == K32_OK);
    std::vector<float> got(k.rows);
    std::memcpy(got.data(), p.mem + kOut, 4 * k.rows);
    *want = reference(w, k.stride, k.bits, ws, x, xs, k.rows, k.cols);
    return got;
}

static void test_imports_compute() {
    Pod p;
    std::vector<float> want;
    // Packed rows (stride 0), then rows with a gap
    for (Case k : {Case{8, 64, 172, 172}, Case{4, 172, 64, 32}, Case{4, 5, 13, 7}}) {
        std::vector<float> got = run_case(p, Case{k.bits, k.rows, k.cols, k.stride}, &want);
        assert(std::memcmp(got.data(), want.data(), 4 * k.rows) == 0);
    }
    std::vector<float> got = run_case(p, Case{8, 20, 30, 40}, &want);
    assert(std::memcmp(got.data(), want.data(), 4 * 20) == 0);
    // stride 0 is a packed matrix: the same as the stride of its rows
    int32_t rc = 1;
    assert(!p.call(8, kOut, kW, kWs, kX, 0.5f, 10, 64, 0, &rc) && rc == K32_OK);
    std::vector<float> packed(10);
    std::memcpy(packed.data(), p.mem + kOut, 40);
    assert(!p.call(8, kOut, kW, kWs, kX, 0.5f, 10, 64, 64, &rc) && rc == K32_OK);
    assert(std::memcmp(packed.data(), p.mem + kOut, 40) == 0);
    // A stride below the bytes of a row
    assert(!p.call(8, kOut, kW, kWs, kX, 0.5f, 4, 64, 63, &rc) && rc == K32_ERR_INVAL);
    assert(!p.call(4, kOut, kW, kWs, kX, 0.5f, 4, 65, 32, &rc) && rc == K32_ERR_INVAL);
    assert(!p.call(4, kOut, kW, kWs, kX, 0.5f, 4, 65, 33, &rc) && rc == K32_OK);

    // The float imports: the kernels' results, through the pod's memory
    std::vector<int8_t> w(40 * 32);
    for (auto& b : w) b = static_cast<int8_t>(rnd() >> 24);
    std::vector<float> ws(40 * 4), q(8), a(40);
    for (auto& f : ws) f = rnd_f32() * 0.01f;
    for (auto& f : q) f = rnd_f32();
    for (auto& f : a) f = rnd_f32();
    std::memcpy(p.mem + kW, w.data(), w.size());
    std::memcpy(p.mem + kWs, ws.data(), 4 * ws.size());
    std::memcpy(p.mem + kX, q.data(), 4 * q.size());
    assert(!p.call_fn(2, &rc, kOut, kW + 8, kWs + 4, kX, 0.25, 40u, 8u, 32u, 16u) && rc == K32_OK);
    std::vector<float> scores = ref_matvec_f32(std::vector<int8_t>(w.begin() + 8, w.end()), 32, std::vector<float>(ws.begin() + 1, ws.end()), 4,
                                               q, 0.25f, 40, 8);
    assert(std::memcmp(p.mem + kOut, scores.data(), 4 * 40) == 0);
    assert(!p.call_fn(4, &rc, kOut, 40u) && rc == K32_OK);
    std::vector<uint8_t> sm(4 * 40);
    std::memcpy(sm.data(), scores.data(), sm.size());
    softmax_f32(sm.data(), 40);
    assert(std::memcmp(p.mem + kOut, sm.data(), sm.size()) == 0);
    std::memcpy(p.mem + kX, a.data(), 4 * a.size());
    assert(!p.call_fn(3, &rc, kOut, kW + 8, kWs + 4, kX, 40u, 8u, 32u, 16u) && rc == K32_OK);
    std::vector<float> mixed = ref_vecmat_f32(std::vector<int8_t>(w.begin() + 8, w.end()), 32, std::vector<float>(ws.begin() + 1, ws.end()), 4,
                                              a, 40, 8);
    assert(std::memcmp(p.mem + kOut, mixed.data(), 4 * 8) == 0);
    // Strides of 0 pack rows and scales; strides below one are refused
    assert(!p.call_fn(2, &rc, kOut, kW, kWs, kX, 1.0, 4u, 8u, 0u, 0u) && rc == K32_OK);
    assert(!p.call_fn(2, &rc, kOut, kW, kWs, kX, 1.0, 4u, 8u, 7u, 0u) && rc == K32_ERR_INVAL);
    assert(!p.call_fn(3, &rc, kOut, kW, kWs, kX, 4u, 8u, 8u, 3u) && rc == K32_ERR_INVAL);
    std::printf("[PASS] test_imports_compute\n");
}

static void expect_trap(Pod& p, int bits, uint32_t out, uint32_t w, uint32_t ws, uint32_t x, uint32_t rows, uint32_t cols, uint32_t stride,
                        const char* operand) {
    std::vector<uint8_t> before(p.mem, p.mem + p.mem_size);
    M3Result r = p.call(bits, out, w, ws, x, 1.0f, rows, cols, stride);
    assert(r);
    const std::string want = std::string("[trap] out of bounds memory access: ") + operand + " of k32_matvec_i" + (bits == 8 ? "8" : "4");
    if (want != r) std::printf("trap: %s\n", r);
    assert(want == r);
    // Nothing written, and the container reports it as a memory fault
    assert(std::memcmp(before.data(), p.mem, p.mem_size) == 0);
    TrapReport t = describe_trap(r, TrapContext{});
    assert(t.exit_code == 139);
    assert(t.message == "wasm trap: " + want.substr(7));
}

static void test_out_of_bounds_traps() {
    const uint32_t end = 2 * 65536;
    {
        Pod p;
        // Each operand just past the end; the call before it just fits
        int32_t rc = 1;
        assert(!p.call(8, end - 4 * 16, end - 16 * 32, end - 4 * 16, end - 32, 1.0f, 16, 32, 0, &rc) && rc == K32_OK);
        expect_trap(p, 8, end - 4 * 16 + 1, kW, kWs, kX, 16, 32, 0, "out");
        expect_trap(p, 8, kOut, end - 16 * 32 + 1, kWs, kX, 16, 32, 0, "w");
        expect_trap(p, 8, kOut, kW, end - 4 * 16 + 1, kX, 16, 32, 0, "w_scale");
        expect_trap(p, 8, kOut, kW, kWs, end - 32 + 1, 16, 32, 0, "x");
    }
    {
        Pod p;
        // int4: a row of 33 columns is 17 bytes; the last row's bytes count, not its stride
        int32_t rc = 1;
        assert(!p.call(4, kOut, end - (3 * 40 + 17), kWs, kX, 1.0f, 4, 33, 40, &rc) && rc == K32_OK);
        expect_trap(p, 4, kOut, end - (3 * 40 + 17) + 1, kWs, kX, 4, 33, 40, "w");
        // Offsets and sizes that wrap 32 bits
        expect_trap(p, 4, 0xFFFFFFF0u, kW, kWs, kX, 8, 8, 0, "out");
        expect_trap(p, 4, kOut, kW, kWs, kX, 0x40000001u, 2, 0, "out");
        expect_trap(p, 4, kOut, 0x10, kWs, kX, 2, 0xFFFFFFFFu, 0, "w");
        expect_trap(p, 4, kOut, kW, kWs, kX, 2, 2, 0xFFFFFFFFu, "w");
        expect_trap(p, 4, kOut, kW, kWs, 0xFFFFFFFFu, 1, 2, 0, "x");
    }
    {
        Pod p;
        // The float imports: the last scale of a strided column, x of x^T W, the softmax's end
        int32_t rc = 1;
        assert(!p.call_fn(2, &rc, kOut, kW, end - (9 * 16 + 4), kX, 1.0, 10u, 8u, 32u, 16u) && rc == K32_OK);
        M3Result r = p.call_fn(2, &rc, kOut, kW, end - (9 * 16 + 3), kX, 1.0, 10u, 8u, 32u, 16u);
        assert(r && std::string(r) == "[trap] out of bounds memory access: w_scale of k32_matvec_f32_i8");
        r = p.call_fn(2, &rc, kOut, kW, kWs, kX, 1.0, 2u, 8u, 32u, 0xFFFFFFFFu);
        assert(r && std::string(r) == "[trap] out of bounds memory access: w_scale of k32_matvec_f32_i8");
        r = p.call_fn(3, &rc, kOut, kW, kWs, end - 4 * 9, 10u, 8u, 32u, 16u);
        assert(r && std::string(r) == "[trap] out of bounds memory access: x of k32_vecmat_f32_i8");
        r = p.call_fn(3, &rc, end - 4 * 7, kW, kWs, kX, 10u, 8u, 32u, 16u);
        assert(r && std::string(r) == "[trap] out of bounds memory access: out of k32_vecmat_f32_i8");
        assert(!p.call_fn(4, &rc, end - 40, 10u) && rc == K32_OK);
        r = p.call_fn(4, &rc, end - 40, 11u);
        assert(r && std::string(r) == "[trap] out of bounds memory access: x of k32_softmax_f32");
        assert(describe_trap(r, TrapContext{}).exit_code == 139);
    }
    std::printf("[PASS] test_out_of_bounds_traps\n");
}

// A pod's call that traps in an import fails the container with exit code 139
// and the import's message, and the node goes on
static void test_pod_trap() {
    auto rt = create_wasm_runtime();
    const std::vector<uint8_t> wasm = linalg_module();
    DeploySpec spec;
    spec.memory_limit_bytes = 2 * 65536;
    std::string err;
    assert(rt->deploy_workload_ex("default/linalg-oob", wasm.data(), wasm.size(), spec, err));
    const int32_t r = rt->execute_function("default/linalg-oob", "oob", {});
    const ContainerStatus st = rt->get_status("default/linalg-oob");
    std::printf("[pod trap] rc %d: %s\n", r, st.exit_message.c_str());
    assert(r == -139 && st.exit_code == 139);
    assert(st.exit_message == "wasm trap: out of bounds memory access: w of k32_matvec_i8");
    rt->terminate("default/linalg-oob");
    std::printf("[PASS] test_pod_trap\n");
}

static void test_gas() {
    const uint64_t budget = 1000000000ull;
    Pod p(budget);
    std::vector<float> want;
    // The segment that calls the import costs the same each time: the
    // difference between two calls of different sizes is the import's gas
    run_case(p, Case{8, 64, 172, 172}, &want);
    const uint64_t first = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits);
    assert(first >= linalg_gas(matrix_units(64, 172)));
    run_case(p, Case{4, 172, 64, 32}, &want);
    const uint64_t second = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits) - first;
    assert(second - linalg_gas(matrix_units(172, 64)) == first - linalg_gas(matrix_units(64, 172)));
    assert(first - linalg_gas(matrix_units(64, 172)) < 2000);
    assert(linalg_gas(matrix_units(64, 172)) == 10000 + 45 * 64 * 173);
    // A softmax pays for its exps
    int32_t rc = 1;
    const uint64_t before = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits);
    assert(!p.call_fn(4, &rc, kOut, 100u) && rc == K32_OK);
    const uint64_t sm = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits) - before;
    assert(sm >= linalg_gas(32 * 100) && sm - linalg_gas(32 * 100) < 1000);

    // A budget that does not cover the work: the call traps before it writes
    m3_SetResourceLimit(p.rt, c_m3Limit_GasUnits, linalg_gas(matrix_units(64, 172)) - 1);
    std::memset(p.mem + kOut, 0x5a, 4 * 64);
    M3Result r = p.call(8, kOut, kW, kWs, kX, 1.0f, 64, 172, 0);
    assert(r == m3Err_trapOutOfGas);
    for (uint32_t i = 0; i < 4 * 64; i++) assert(p.mem[kOut + i] == 0x5a);
    TrapContext ctx;
    ctx.gas_budget = linalg_gas(matrix_units(64, 172)) - 1;
    assert(describe_trap(r, ctx).exit_code == 152);
    // One that does
    m3_SetResourceLimit(p.rt, c_m3Limit_GasUnits, linalg_gas(matrix_units(64, 172)) + 5000);
    assert(!p.call(8, kOut, kW, kWs, kX, 1.0f, 64, 172, 0, &rc) && rc == K32_OK);

    // An unmetered pod pays nothing
    Pod free_pod;
    run_case(free_pod, Case{8, 64, 172, 172}, &want);
    assert(m3_GetResourceUsage(free_pod.rt, c_m3Limit_GasUnits) == 0);
    std::printf("[PASS] test_gas\n");
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

// Weights and scales read in place from an open blob (K32_BLOB_ADDR) give
// the bits they give from linear memory; a blob address outside every open
// blob traps, and a blob that is not mapped answers K32_ERR_NOTSUP
static void test_blob_operands() {
    using namespace kube32;
    const std::string pod = "default/linalg";
    storage::FlashBlobStore flash(storage::create_ram_blob_region(512 * 1024), false);
    storage::FlashBlobStore unmapped(std::make_unique<UnmappedRegion>(512 * 1024), false);
    Pod p;
    for (Case k : {Case{8, 64, 172, 172}, Case{4, 172, 64, 32}}) {
        std::vector<int8_t> w, x;
        std::vector<float> ws;
        float xs;
        fill(k, w, ws, x, xs);
        // One blob: the rows, then the scales at an odd offset
        std::vector<uint8_t> bytes(w.size() + 1 + 4 * ws.size());
        std::memcpy(bytes.data(), w.data(), w.size());
        std::memcpy(bytes.data() + w.size() + 1, ws.data(), 4 * ws.size());
        store_blob(flash, "w", bytes);
        store_blob(unmapped, "w", bytes);
        storage::set_node_blob_stores(&flash, nullptr);
        PodBlobs::instance().set_allowed(pod, {"w"});
        const int32_t h = PodBlobs::instance().open(pod, "w");
        assert(h == 0 && PodBlobs::instance().mapped(pod, h) == 1);
        std::memcpy(p.mem + kX, x.data(), x.size());
        const uint32_t wa = blob_addr(h, 0), sa = blob_addr(h, static_cast<uint32_t>(w.size() + 1));
        int32_t rc = 1;
        assert(!p.call(k.bits, kOut, wa, sa, kX, xs, k.rows, k.cols, static_cast<uint32_t>(k.stride), &rc) && rc == K32_OK);
        const std::vector<float> want = reference(w, k.stride, k.bits, ws, x, xs, k.rows, k.cols);
        assert(std::memcmp(p.mem + kOut, want.data(), 4 * k.rows) == 0);
        // Scales from memory, weights from the blob
        std::memcpy(p.mem + kWs, ws.data(), 4 * ws.size());
        std::memset(p.mem + kOut, 0, 4 * k.rows);
        assert(!p.call(k.bits, kOut, wa, kWs, kX, xs, k.rows, k.cols, static_cast<uint32_t>(k.stride), &rc) && rc == K32_OK);
        assert(std::memcmp(p.mem + kOut, want.data(), 4 * k.rows) == 0);
        // The last scale one byte past the blob, a handle not open
        const std::string fn = std::string("k32_matvec_i") + (k.bits == 8 ? "8" : "4");
        M3Result r = p.call(k.bits, kOut, wa, blob_addr(h, static_cast<uint32_t>(w.size() + 2)), kX, xs, k.rows, k.cols,
                            static_cast<uint32_t>(k.stride));
        assert(r && std::string(r) == "[trap] out of bounds memory access: w_scale of " + fn + " (a blob address outside every open blob)");
        assert(describe_trap(r, TrapContext{}).exit_code == 139);
        r = p.call(k.bits, kOut, blob_addr(1, 0), kWs, kX, xs, k.rows, k.cols, static_cast<uint32_t>(k.stride));
        assert(r && std::string(r) == "[trap] out of bounds memory access: w of " + fn + " (a blob address outside every open blob)");
        PodBlobs::instance().forget_pod(pod);
        // The same blob where it is not mapped
        storage::set_node_blob_stores(&unmapped, nullptr);
        PodBlobs::instance().set_allowed(pod, {"w"});
        assert(PodBlobs::instance().open(pod, "w") == 0 && PodBlobs::instance().mapped(pod, 0) == 0);
        rc = 1;
        assert(!p.call(k.bits, kOut, wa, sa, kX, xs, k.rows, k.cols, static_cast<uint32_t>(k.stride), &rc) && rc == K32_ERR_NOTSUP);
        PodBlobs::instance().forget_pod(pod);
    }
    storage::set_node_blob_stores(nullptr, nullptr);
    std::printf("[PASS] test_blob_operands\n");
}

// k32_matmul_i8 and k32_matmul_i4 through a pod: n results, the gas of n
// products, operands outside the memory, weights read from a blob in place
static void test_matmul_import() {
    using namespace kube32;
    constexpr uint32_t kXs = 0xB000, n = 5;
    for (Case k : {Case{8, 64, 172, 0}, Case{4, 172, 64, 40}}) {
        const uint64_t budget = 1000000000ull;
        Pod p(budget);
        const uint32_t row_bytes = k.bits == 8 ? k.cols : (k.cols + 1) / 2;
        const size_t stride = k.stride ? k.stride : row_bytes;
        std::vector<int8_t> w, x0;
        std::vector<float> ws;
        float xs0;
        fill(Case{k.bits, k.rows, k.cols, stride}, w, ws, x0, xs0);
        std::vector<int8_t> x(n * k.cols);
        for (auto& v : x) v = static_cast<int8_t>(rnd() >> 24);
        float xs[n];
        for (float& f : xs) f = static_cast<float>(rnd() >> 8) / 16777216.0f * 0.1f;
        std::memcpy(p.mem + kW, w.data(), w.size());
        std::memcpy(p.mem + kWs, ws.data(), 4 * ws.size());
        std::memcpy(p.mem + kX, x.data(), x.size());
        std::memcpy(p.mem + kXs, xs, sizeof xs);
        const int f = k.bits == 8 ? 5 : 6;
        const std::string fn = std::string("k32_matmul_i") + (k.bits == 8 ? "8" : "4");
        int32_t rc = 1;
        const uint64_t before = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits);
        assert(!p.call_fn(f, &rc, kOut, kW, kWs, kX, kXs, k.rows, k.cols, static_cast<uint32_t>(k.stride), n) && rc == K32_OK);
        const uint64_t gas = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits) - before;
        assert(gas >= linalg_gas(n * matrix_units(k.rows, k.cols)) && gas - linalg_gas(n * matrix_units(k.rows, k.cols)) < 2000);
        for (uint32_t i = 0; i < n; i++) {
            const std::vector<int8_t> xp(x.begin() + i * k.cols, x.begin() + (i + 1) * k.cols);
            const std::vector<float> want = reference(w, stride, k.bits, ws, xp, xs[i], k.rows, k.cols);
            assert(std::memcmp(p.mem + kOut + 4 * i * k.rows, want.data(), 4 * k.rows) == 0);
        }
        // n = 0 writes nothing; a stride below a row's bytes is refused
        assert(!p.call_fn(f, &rc, kOut, kW, kWs, kX, kXs, k.rows, k.cols, 0u, 0u) && rc == K32_OK);
        assert(!p.call_fn(f, &rc, kOut, kW, kWs, kX, kXs, k.rows, k.cols, row_bytes - 1, n) && rc == K32_ERR_INVAL);
        // Each operand past the end of the memory
        const uint32_t end = 2 * 65536;
        const auto trap = [&](uint32_t out, uint32_t wa, uint32_t sa, uint32_t xa, uint32_t xsa, const char* operand) {
            M3Result r = p.call_fn(f, &rc, out, wa, sa, xa, xsa, k.rows, k.cols, static_cast<uint32_t>(k.stride), n);
            assert(r && std::string(r) == "[trap] out of bounds memory access: " + std::string(operand) + " of " + fn);
            assert(describe_trap(r, TrapContext{}).exit_code == 139);
        };
        trap(end - 4 * n * k.rows + 1, kW, kWs, kX, kXs, "out");
        trap(kOut, end - static_cast<uint32_t>((k.rows - 1) * stride + row_bytes) + 1, kWs, kX, kXs, "w");
        trap(kOut, kW, end - 4 * k.rows + 1, kX, kXs, "w_scale");
        trap(kOut, kW, kWs, end - n * k.cols + 1, kXs, "x");
        trap(kOut, kW, kWs, kX, end - 4 * n + 1, "x_scale");
        // The weights and their scales from a blob, in place
        storage::FlashBlobStore flash(storage::create_ram_blob_region(512 * 1024), false);
        std::vector<uint8_t> bytes(w.size() + 4 * ws.size());
        std::memcpy(bytes.data(), w.data(), w.size());
        std::memcpy(bytes.data() + w.size(), ws.data(), 4 * ws.size());
        store_blob(flash, "w", bytes);
        storage::set_node_blob_stores(&flash, nullptr);
        const std::string pod = "default/linalg";
        PodBlobs::instance().set_allowed(pod, {"w"});
        const int32_t h = PodBlobs::instance().open(pod, "w");
        assert(h == 0);
        std::memset(p.mem + kOut, 0, 4 * n * k.rows);
        assert(!p.call_fn(f, &rc, kOut, blob_addr(h, 0), blob_addr(h, static_cast<uint32_t>(w.size())), kX, kXs, k.rows, k.cols,
                          static_cast<uint32_t>(k.stride), n) &&
               rc == K32_OK);
        for (uint32_t i = 0; i < n; i++) {
            const std::vector<int8_t> xp(x.begin() + i * k.cols, x.begin() + (i + 1) * k.cols);
            const std::vector<float> want = reference(w, stride, k.bits, ws, xp, xs[i], k.rows, k.cols);
            assert(std::memcmp(p.mem + kOut + 4 * i * k.rows, want.data(), 4 * k.rows) == 0);
        }
        M3Result r = p.call_fn(f, &rc, kOut, blob_addr(h, 0), blob_addr(h, static_cast<uint32_t>(w.size()) + 1), kX, kXs, k.rows, k.cols,
                               static_cast<uint32_t>(k.stride), n);
        assert(r && std::string(r) == "[trap] out of bounds memory access: w_scale of " + fn + " (a blob address outside every open blob)");
        PodBlobs::instance().forget_pod(pod);
        storage::set_node_blob_stores(nullptr, nullptr);
    }
    std::printf("[PASS] test_matmul_import\n");
}

// ---- element-wise operations ----------------------------------------------------

// The steps as llm-shard wrote them in Wasm before the imports (and as
// tools/llm.py repeats them)
static void ref_rmsnorm(float* o, const float* x, const float* w, uint32_t n, float eps) {
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / static_cast<float>(n);
    ss = ss + eps;
    ss = 1.0f / std::sqrt(ss);
    for (uint32_t i = 0; i < n; i++) o[i] = w[i] * (ss * x[i]);
}

static float ref_quantize(int8_t* q, const float* x, uint32_t n) {
    float amax = 0.0f;
    for (uint32_t i = 0; i < n; i++) amax = std::fabs(x[i]) > amax ? std::fabs(x[i]) : amax;
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
    for (uint32_t i = 0; i < n; i++) {
        const float t = x[i] * inv;
        const int32_t r = static_cast<int32_t>(t + (t >= 0.0f ? 0.5f : -0.5f));
        q[i] = static_cast<int8_t>(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    return amax / 127.0f;
}

static void ref_rope(float* a, const float* cs, uint32_t n, uint32_t hs) {
    for (uint32_t i = 0; i < n; i += 2) {
        const float c = cs[i % hs], s = cs[i % hs + 1];
        const float a0 = a[i], a1 = a[i + 1];
        a[i] = a0 * c - a1 * s;
        a[i + 1] = a0 * s + a1 * c;
    }
}

static uint8_t* bytes_of(std::vector<float>& v) { return reinterpret_cast<uint8_t*>(v.data()); }

static void test_elementwise_kernels() {
    for (uint32_t n : {1u, 2u, 7u, 64u, 172u, 256u})
        for (uint32_t count : {1u, 3u}) {
            const uint32_t m = n * count;
            std::vector<float> x(m), w(n), got(m), want(m), g(m);
            for (uint32_t i = 0; i < m; i++) x[i] = rnd_f32() * (i % 3 ? 1.0f : 37.0f);
            for (uint32_t i = 0; i < n; i++) w[i] = rnd_f32();
            for (float& v : g) v = rnd_f32() * 4.0f;
            // rmsnorm, then in place
            rmsnorm_f32(bytes_of(got), bytes_of(x), bytes_of(w), n, count, 1e-5f);
            for (uint32_t p = 0; p < count; p++) ref_rmsnorm(want.data() + p * n, x.data() + p * n, w.data(), n, 1e-5f);
            assert(std::memcmp(got.data(), want.data(), 4 * m) == 0);
            got = x;
            rmsnorm_f32(bytes_of(got), bytes_of(got), bytes_of(w), n, count, 1e-5f);
            assert(std::memcmp(got.data(), want.data(), 4 * m) == 0);
            // quantize: each vector with its scale
            std::vector<int8_t> q(m), qr(m);
            std::vector<float> s(count);
            quantize_i8(q.data(), reinterpret_cast<uint8_t*>(s.data()), bytes_of(x), n, count);
            for (uint32_t p = 0; p < count; p++) {
                const float sr = ref_quantize(qr.data() + p * n, x.data() + p * n, n);
                assert(std::memcmp(&s[p], &sr, 4) == 0);
            }
            assert(q == qr);
            // rope with head size 2 (any even n) and n itself
            if (n % 2 == 0)
                for (uint32_t hs : {2u, n}) {
                    std::vector<float> cs(hs * count);
                    for (float& v : cs) v = rnd_f32();
                    got = want = x;
                    rope_f32(bytes_of(got), bytes_of(cs), n, hs, count);
                    for (uint32_t p = 0; p < count; p++) ref_rope(want.data() + p * n, cs.data() + p * hs, n, hs);
                    assert(std::memcmp(got.data(), want.data(), 4 * m) == 0);
                }
            // swiglu, add
            got = x;
            swiglu_f32(bytes_of(got), bytes_of(g), m);
            for (uint32_t i = 0; i < m; i++) {
                float a = x[i];
                a = a * (1.0f / (1.0f + k32_expf(-a)));
                want[i] = a * g[i];
            }
            assert(std::memcmp(got.data(), want.data(), 4 * m) == 0);
            add_f32(bytes_of(got), bytes_of(g), m);
            for (uint32_t i = 0; i < m; i++) want[i] += g[i];
            assert(std::memcmp(got.data(), want.data(), 4 * m) == 0);
            // argmax: the first of equal ones
            x[m / 2] = x[m - 1] = 1000.0f;
            assert(argmax_f32(bytes_of(x), m) == m / 2);
        }
    // A vector of zeros: q 0, scale 0; NaN gives 0, an infinite factor the clamp
    std::vector<float> z(8, 0.0f), s(2);
    std::vector<int8_t> q(16, 5);
    quantize_i8(q.data(), reinterpret_cast<uint8_t*>(s.data()), bytes_of(z), 8, 1);
    assert(s[0] == 0.0f && std::count(q.begin(), q.begin() + 8, 0) == 8);
    std::vector<float> odd = {std::nanf(""), 1.0f, -2.0f, 1e-40f};
    quantize_i8(q.data(), reinterpret_cast<uint8_t*>(s.data()), bytes_of(odd), 4, 1);
    assert(q[0] == 0 && q[1] == 64 && q[2] == -127 && q[3] == 0);
    std::vector<float> tiny = {1e-40f, -1e-40f};   // 127 / max|x| is infinite
    quantize_i8(q.data(), reinterpret_cast<uint8_t*>(s.data()), bytes_of(tiny), 2, 1);
    assert(q[0] == 127 && q[1] == -127);
    assert(argmax_f32(bytes_of(z), 0) == 0);
    std::printf("[PASS] test_elementwise_kernels\n");
}

// The element-wise imports through a pod: the kernels' bits, operands
// outside the memory, a rope of odd sizes, gas, and the weights of rmsnorm
// and the table of rope read in place from a blob
static void test_elementwise_imports() {
    using namespace kube32;
    enum { RMS = 7, QNT = 8, ROPE = 9, SWI = 10, ADD = 11, AMAX = 12 };
    constexpr uint32_t kA = 0x1000, kB = 0x3000, kC = 0x5000, kQ = 0x7000, kS = 0x7800;
    constexpr uint32_t n = 64, count = 3, hs = 16, m = n * count;
    Pod p(1000000000ull);
    std::vector<float> x(m), w(n), cs(hs * count), g(m);
    for (float& v : x) v = rnd_f32() * 8.0f;
    for (float& v : w) v = rnd_f32();
    for (float& v : cs) v = rnd_f32();
    for (float& v : g) v = rnd_f32();
    std::memcpy(p.mem + kA, x.data(), 4 * m);
    std::memcpy(p.mem + kB, w.data(), 4 * n);
    std::memcpy(p.mem + kC + 1, g.data(), 4 * m);   // unaligned
    int32_t rc = 1;
    const auto gas = [&](auto call, uint64_t units) {
        const uint64_t before = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits);
        call();
        const uint64_t used = m3_GetResourceUsage(p.rt, c_m3Limit_GasUnits) - before;
        assert(used >= linalg_gas(units) && used - linalg_gas(units) < 1000);
    };
    // rmsnorm into kC + 1 + 4 m
    std::vector<float> want(m);
    rmsnorm_f32(bytes_of(want), bytes_of(x), bytes_of(w), n, count, 1e-5f);
    const uint32_t out = kC + 1 + 4 * m;
    gas([&] { assert(!p.call_fn(RMS, &rc, out, kA, kB, n, count, 1e-5) && rc == K32_OK); }, kRmsnormUnits * m);
    assert(std::memcmp(p.mem + out, want.data(), 4 * m) == 0);
    // quantize of the same vectors
    std::vector<int8_t> qw(m);
    std::vector<float> sw(count);
    quantize_i8(qw.data(), reinterpret_cast<uint8_t*>(sw.data()), bytes_of(x), n, count);
    gas([&] { assert(!p.call_fn(QNT, &rc, kQ, kS, kA, n, count) && rc == K32_OK); }, kQuantizeUnits * m);
    assert(std::memcmp(p.mem + kQ, qw.data(), m) == 0 && std::memcmp(p.mem + kS, sw.data(), 4 * count) == 0);
    // rope, in place at kA, with the table at kB + 4 n
    std::memcpy(p.mem + kB + 4 * n, cs.data(), 4 * cs.size());
    want = x;
    rope_f32(bytes_of(want), bytes_of(cs), n, hs, count);
    gas([&] { assert(!p.call_fn(ROPE, &rc, kA, kB + 4 * n, n, hs, count) && rc == K32_OK); }, kRopeUnits * m);
    assert(std::memcmp(p.mem + kA, want.data(), 4 * m) == 0);
    for (uint32_t bad : {0u, 1u})
        assert(!p.call_fn(ROPE, &rc, kA, kB, bad ? n + 1 : n, bad ? hs : 0u, count) && rc == K32_ERR_INVAL);
    assert(!p.call_fn(ROPE, &rc, kA, kB, n, hs + 1, count) && rc == K32_ERR_INVAL);
    // swiglu and add over the rotated vectors, the gate unaligned
    std::vector<float> h = want;
    swiglu_f32(bytes_of(h), bytes_of(g), m);
    gas([&] { assert(!p.call_fn(SWI, &rc, kA, kC + 1, m) && rc == K32_OK); }, kSwigluUnits * m);
    assert(std::memcmp(p.mem + kA, h.data(), 4 * m) == 0);
    add_f32(bytes_of(h), bytes_of(g), m);
    gas([&] { assert(!p.call_fn(ADD, &rc, kA, kC + 1, m) && rc == K32_OK); }, kAddUnits * m);
    assert(std::memcmp(p.mem + kA, h.data(), 4 * m) == 0);
    // argmax answers the index
    const uint32_t best = argmax_f32(bytes_of(h), m);
    gas([&] { assert(!p.call_fn(AMAX, &rc, kA, m) && rc == static_cast<int32_t>(best)); }, kArgmaxUnits * m);

    // Each operand one float past the end of the memory
    const uint32_t end = 2 * 65536;
    const auto trap = [&](M3Result r, const char* operand, const char* fn) {
        assert(r && std::string(r) == "[trap] out of bounds memory access: " + std::string(operand) + " of " + fn);
        assert(describe_trap(r, TrapContext{}).exit_code == 139);
    };
    trap(p.call_fn(RMS, &rc, end - 4 * m + 1, kA, kB, n, count, 1e-5), "out", "k32_rmsnorm_f32");
    trap(p.call_fn(RMS, &rc, out, end - 4 * m + 1, kB, n, count, 1e-5), "x", "k32_rmsnorm_f32");
    trap(p.call_fn(RMS, &rc, out, kA, end - 4 * n + 1, n, count, 1e-5), "w", "k32_rmsnorm_f32");
    trap(p.call_fn(QNT, &rc, end - m + 1, kS, kA, n, count), "q", "k32_quantize_i8");
    trap(p.call_fn(QNT, &rc, kQ, end - 4 * count + 1, kA, n, count), "scale", "k32_quantize_i8");
    trap(p.call_fn(QNT, &rc, kQ, kS, end - 4 * m + 1, n, count), "x", "k32_quantize_i8");
    trap(p.call_fn(ROPE, &rc, end - 4 * m + 1, kB, n, hs, count), "x", "k32_rope_f32");
    trap(p.call_fn(ROPE, &rc, kA, end - 4 * hs * count + 1, n, hs, count), "cs", "k32_rope_f32");
    trap(p.call_fn(SWI, &rc, end - 4 * m + 1, kC, m), "x", "k32_swiglu_f32");
    trap(p.call_fn(SWI, &rc, kA, end - 4 * m + 1, m), "g", "k32_swiglu_f32");
    trap(p.call_fn(ADD, &rc, end - 4 * m + 1, kC, m), "x", "k32_add_f32");
    trap(p.call_fn(ADD, &rc, kA, end - 4 * m + 1, m), "y", "k32_add_f32");
    trap(p.call_fn(AMAX, &rc, end - 4 * m + 1, m), "x", "k32_argmax_f32");
    // A call its budget does not cover traps before it writes
    m3_SetResourceLimit(p.rt, c_m3Limit_GasUnits, linalg_gas(kRmsnormUnits * m) - 1);
    std::memset(p.mem + out, 0x5a, 4 * m);
    assert(p.call_fn(RMS, &rc, out, kA, kB, n, count, 1e-5) == m3Err_trapOutOfGas);
    for (uint32_t i = 0; i < 4 * m; i++) assert(p.mem[out + i] == 0x5a);

    // The norm's weights and the rotary table in place from a blob
    Pod b;
    storage::FlashBlobStore flash(storage::create_ram_blob_region(512 * 1024), false);
    std::vector<uint8_t> blob(3 + 4 * n + 4 * cs.size());
    std::memcpy(blob.data() + 3, w.data(), 4 * n);
    std::memcpy(blob.data() + 3 + 4 * n, cs.data(), 4 * cs.size());
    store_blob(flash, "norms", blob);
    storage::set_node_blob_stores(&flash, nullptr);
    const std::string pod = "default/linalg";
    PodBlobs::instance().set_allowed(pod, {"norms"});
    const int32_t hd = PodBlobs::instance().open(pod, "norms");
    assert(hd == 0);
    std::memcpy(b.mem + kA, x.data(), 4 * m);
    rmsnorm_f32(bytes_of(want), bytes_of(x), bytes_of(w), n, count, 1e-5f);
    assert(!b.call_fn(RMS, &rc, kC, kA, blob_addr(hd, 3), n, count, 1e-5) && rc == K32_OK);
    assert(std::memcmp(b.mem + kC, want.data(), 4 * m) == 0);
    rope_f32(bytes_of(want), bytes_of(cs), n, hs, count);
    assert(!b.call_fn(ROPE, &rc, kC, blob_addr(hd, 3 + 4 * n), n, hs, count) && rc == K32_OK);
    assert(std::memcmp(b.mem + kC, want.data(), 4 * m) == 0);
    M3Result r = b.call_fn(ROPE, &rc, kC, blob_addr(hd, 4 + 4 * n), n, hs, count);
    assert(r && std::string(r) == "[trap] out of bounds memory access: cs of k32_rope_f32 (a blob address outside every open blob)");
    r = b.call_fn(RMS, &rc, kC, kA, blob_addr(1, 0), n, count, 1e-5);
    assert(r && std::string(r) == "[trap] out of bounds memory access: w of k32_rmsnorm_f32 (a blob address outside every open blob)");
    PodBlobs::instance().forget_pod(pod);
    storage::set_node_blob_stores(nullptr, nullptr);
    std::printf("[PASS] test_elementwise_imports\n");
}

int main() {
    test_kernels_match_reference();
    test_matmul_kernels();
    test_float_kernels();
    test_imports_compute();
    test_out_of_bounds_traps();
    test_pod_trap();
    test_gas();
    test_blob_operands();
    test_matmul_import();
    test_elementwise_kernels();
    test_elementwise_imports();
    std::printf("All linear algebra tests passed\n");
    return 0;
}
