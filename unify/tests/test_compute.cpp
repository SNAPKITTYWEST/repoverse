#include "test_main.h"
#include "../engine/wasm/compute.h"
#include "../engine/serialization/stream.h"

using namespace unify;

static const char* kVadd = R"(
__global__ void vadd(const float* A, const float* B, float* C, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        C[i] = A[i] + B[i];
    }
}
)";

TEST(kernel_pipeline_produces_ast_ir_and_wasm) {
    kernel::CompiledKernel k = kernel::compile(kVadd);
    CHECK(k.ok);
    CHECK(k.name == "vadd");
    CHECK(k.params.size() == 4);
    CHECK(k.ir_text.find("store.f32 %p2[%i] = (load.f32 %p0[%i] +.f32 load.f32 %p1[%i])") != std::string::npos);
    wasm::Module m;
    std::string err;
    CHECK(wasm::decode(k.wasm, m, err));
    CHECK(m.memory_imported);
    CHECK(m.functions.size() == 2);
}

TEST(kernel_vadd_executes_in_wasm_runtime) {
    ComputeRuntime rt;
    std::string err;
    KernelHandle k = rt.compile_and_load(kVadd, err);
    CHECK(k);
    const int n = 1000;
    std::vector<float> a(n), b(n), c(n, -1);
    for (int i = 0; i < n; i++) { a[size_t(i)] = float(i) * 0.5f; b[size_t(i)] = 100.0f - float(i); }
    BufferHandle A = rt.buffer_create(n * 4), B = rt.buffer_create(n * 4), C = rt.buffer_create(n * 4);
    CHECK(rt.buffer_upload(A, a.data(), n * 4));
    CHECK(rt.buffer_upload(B, b.data(), n * 4));
    // 4 blocks of 256 threads = 1024 threads; the bounds check in the kernel skips the extra 24.
    DispatchResult r = rt.dispatch(k, 4, 256, {KernelArg::buf(A), KernelArg::buf(B), KernelArg::buf(C), KernelArg::integer(n)});
    CHECK(r.ok);
    CHECK(rt.buffer_download(C, c.data(), n * 4));
    bool all = true;
    for (int i = 0; i < n; i++) all &= c[size_t(i)] == a[size_t(i)] + b[size_t(i)];
    CHECK(all);
}

TEST(kernel_loops_locals_intrinsics_and_casts) {
    const char* src = R"(
    __global__ void stats(const float* x, float* out, int n) {
        if (threadIdx.x != 0) return;
        float sum = 0.0f;
        float hi = -1000000.0f;
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (x[i] < 0.0f) continue;
            sum += x[i];
            hi = fmaxf(hi, x[i]);
            count = count + 1;
            if (count == 4) break;
        }
        out[0] = sum;
        out[1] = hi;
        out[2] = (float)count;
        out[3] = sqrtf(16.0f) + 2 * 3;
    })";
    ComputeRuntime rt;
    std::string err;
    KernelHandle k = rt.compile_and_load(src, err);
    CHECK(k);
    if (!k) std::printf("    compile error: %s\n", err.c_str());
    float in[] = {1, -5, 2, 9, -1, 3, 100};
    BufferHandle X = rt.buffer_create(sizeof in), O = rt.buffer_create(16);
    rt.buffer_upload(X, in, sizeof in);
    CHECK(rt.dispatch(k, 1, 1, {KernelArg::buf(X), KernelArg::buf(O), KernelArg::integer(7)}).ok);
    float out[4];
    rt.buffer_download(O, out, 16);
    CHECK_NEAR(out[0], 1 + 2 + 9 + 3, 0);  // negative skipped by continue; stops at 4 values
    CHECK_NEAR(out[1], 9, 0);
    CHECK_NEAR(out[2], 4, 0);
    CHECK_NEAR(out[3], 10, 0);             // constant-folded at compile time
    CHECK(rt.kernel_info(k)->folded_constants > 0);
}

TEST(kernel_compile_errors_have_line_numbers) {
    auto k = kernel::compile("__global__ void f(float* a) {\n  a[0] = b;\n}");
    CHECK(!k.ok);
    CHECK(k.error.find("line 2") != std::string::npos);
    CHECK(k.error.find("'b'") != std::string::npos);
    auto k2 = kernel::compile("__global__ void f(float* a) { a = 1; }");
    CHECK(!k2.ok);
}

TEST(kernel_out_of_bounds_traps_instead_of_crashing) {
    ComputeRuntime rt(1, 1);
    std::string err;
    KernelHandle k = rt.compile_and_load("__global__ void w(float* a, int i) { a[i] = 1.0f; }", err);
    BufferHandle A = rt.buffer_create(16);
    DispatchResult r = rt.dispatch(k, 1, 1, {KernelArg::buf(A), KernelArg::integer(1 << 24)});
    CHECK(!r.ok);
    CHECK(r.error.find("out of bounds") != std::string::npos);
    DispatchResult r2 = rt.dispatch(k, 1, 1, {KernelArg::integer(3), KernelArg::integer(0)});
    CHECK(!r2.ok);  // scalar passed where a buffer is required
}

TEST(kernel_handles_are_validated) {
    ComputeRuntime rt;
    BufferHandle b = rt.buffer_create(64);
    CHECK(rt.buffer_destroy(b));
    float x = 1;
    CHECK(!rt.buffer_upload(b, &x, 4));        // stale handle
    CHECK(!rt.dispatch(KernelHandle{}, 1, 1, {}).ok);
    BufferHandle b2 = rt.buffer_create(64);
    CHECK(b2.index() == b.index() && b2 != b);  // slot reused, generation bumped
}

TEST(wasm_interpreter_handles_loops_calls_and_traps) {
    // Hand-assembled module: (func $fact (param i32) (result i32) recursive) and (func $div (param i32 i32) (result i32)).
    std::vector<uint8_t> m = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x0C, 0x02, 0x60, 0x01, 0x7F, 0x01, 0x7F, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,  // types
        0x03, 0x03, 0x02, 0x00, 0x01,                                                        // funcs
        0x07, 0x0E, 0x02, 0x04, 'f', 'a', 'c', 't', 0x00, 0x00, 0x03, 'd', 'i', 'v', 0x00, 0x01,
        0x0A, 0x21, 0x02,
        // fact: if (n <= 1) 1 else n * fact(n-1)
        0x17, 0x00, 0x20, 0x00, 0x41, 0x01, 0x4C, 0x04, 0x7F, 0x41, 0x01, 0x05, 0x20, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6B, 0x10, 0x00, 0x6C, 0x0B, 0x0B,
        // div: a / b (signed)
        0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6D, 0x0B};
    auto mod = std::make_shared<wasm::Module>();
    std::string err;
    CHECK(wasm::decode(m, *mod, err));
    if (!err.empty()) std::printf("    decode error: %s\n", err.c_str());
    wasm::Instance inst;
    CHECK(inst.instantiate(mod, nullptr, {}, err));
    auto r = inst.call("fact", {wasm::Value::i32(10)});
    CHECK(r.ok && r.results.size() == 1 && r.results[0].as_i32() == 3628800);
    auto d = inst.call("div", {wasm::Value::i32(7), wasm::Value::i32(0)});
    CHECK(!d.ok && d.trap == "integer divide by zero");
    inst.max_call_depth = 5;
    CHECK(!inst.call("fact", {wasm::Value::i32(50)}).ok);
    inst.max_call_depth = 512;
    inst.fuel = 20;
    CHECK(inst.call("fact", {wasm::Value::i32(10)}).trap == "fuel exhausted");
}

TEST(wasm_rejects_malformed_modules) {
    wasm::Module m;
    std::string err;
    CHECK(!wasm::decode({0x00, 'a', 's', 'm'}, m, err));
    CHECK(!wasm::decode({0x00, 'a', 's', 'm', 1, 0, 0, 0, 0x0A, 0x05, 0x01, 0x03, 0x00, 0xFF, 0x0B}, m, err));
}

TEST(compute_state_round_trips) {
    ComputeRuntime rt;
    std::string err;
    KernelHandle k = rt.compile_and_load(kVadd, err);
    BufferHandle A = rt.buffer_create(16), B = rt.buffer_create(16), C = rt.buffer_create(16);
    float a[4] = {1, 2, 3, 4}, b[4] = {10, 20, 30, 40};
    rt.buffer_upload(A, a, 16); rt.buffer_upload(B, b, 16);
    BinaryWriter w;
    rt.serialize(w);
    ComputeRuntime restored;
    BinaryReader r(w.data());
    CHECK(restored.deserialize(r, err));
    CHECK(restored.buffer_valid(A) && restored.kernel_valid(k));
    CHECK(restored.dispatch(k, 1, 4, {KernelArg::buf(A), KernelArg::buf(B), KernelArg::buf(C), KernelArg::integer(4)}).ok);
    float c[4];
    restored.buffer_download(C, c, 16);
    CHECK(c[0] == 11 && c[3] == 44);
    BufferHandle D = restored.buffer_create(16);  // allocator state restored: no overlap with live buffers
    CHECK(D.index() == 3);
}
