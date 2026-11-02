// The pods' linear algebra imports (k32_matvec_*, k32_vecmat_f32_i8,
// k32_softmax_f32): the native kernels against plain reference loops and the
// reference's bits, and the imports as a pod calls them: results, strides,
// operands outside the linear memory, a trapped container, and the gas they
// charge.
#include "runtime/pod_linalg.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/container_lifecycle.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/k32_app_abi.h"

#include "wasm3.h"
#include "m3_env.h"

#include <cassert>
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

// The imports, with functions "mv8", "mv4", "mvf", "vm" and "sm" that pass
// their arguments on, "oob" that calls k32_matvec_i8 with w past the end of
// memory, and two pages of memory
constexpr uint8_t kI32 = 0x7f, kF32 = 0x7d;
static const std::vector<std::vector<uint8_t>> kParams = {{kI32, kI32, kI32, kI32, kF32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32, kI32, kF32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32, kI32, kI32, kI32, kI32, kI32, kI32},
                                                          {kI32, kI32},
                                                          {}};
static const char* const kImports[] = {"k32_matvec_i8", "k32_matvec_i4", "k32_matvec_f32_i8", "k32_vecmat_f32_i8", "k32_softmax_f32"};
static const uint8_t kImportType[] = {0, 0, 1, 2, 3};

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
    std::vector<uint8_t> imports = {5};
    for (int i = 0; i < 5; i++) {
        name(imports, "env");
        name(imports, kImports[i]);
        imports.push_back(0x00);
        imports.push_back(kImportType[i]);
    }
    section(m, 2, imports);
    section(m, 3, {0x06, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04});
    section(m, 5, {0x01, 0x01, 0x02, 0x02});
    std::vector<uint8_t> exports = {7};
    const char* const fns[] = {"mv8", "mv4", "mvf", "vm", "sm", "oob"};
    for (uint8_t i = 0; i < 6; i++) {
        name(exports, fns[i]);
        exports.push_back(0x00);
        exports.push_back(5 + i);
    }
    name(exports, "memory");
    exports.push_back(0x02);
    exports.push_back(0x00);
    section(m, 7, exports);
    std::vector<uint8_t> code = {6};
    for (uint8_t callee = 0; callee < 6; callee++) {
        std::vector<uint8_t> body = {0x00};
        if (callee < 5) {
            for (uint8_t i = 0; i < kParams[kImportType[callee]].size(); i++) { body.push_back(0x20); body.push_back(i); }
            body.push_back(0x10); body.push_back(callee);
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
    IM3Function fn[5]{};
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
        const char* const names[] = {"mv8", "mv4", "mvf", "vm", "sm"};
        for (int i = 0; i < 5 && !r; i++) r = m3_FindFunction(&fn[i], rt, names[i]);
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

int main() {
    test_kernels_match_reference();
    test_float_kernels();
    test_imports_compute();
    test_out_of_bounds_traps();
    test_pod_trap();
    test_gas();
    std::printf("All linear algebra tests passed\n");
    return 0;
}
