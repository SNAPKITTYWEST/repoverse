#pragma once
#include <memory>
#include <string>
#include <vector>
#include "wasm.h"
#include "../core/handle.h"
#include "../memory/memory.h"
#include "../kernel/kernel.h"

namespace unify {
class BinaryWriter;
class BinaryReader;

/// A kernel argument: a buffer handle (passed to WASM as its byte offset) or a scalar.
struct KernelArg {
    enum Kind { Buffer, Int, Float } kind;
    uint32_t buffer = 0;   // BufferHandle bits
    int32_t i = 0;
    float f = 0;
    static KernelArg buf(BufferHandle h) { return {Buffer, h.bits, 0, 0}; }
    static KernelArg integer(int32_t v) { return {Int, 0, v, 0}; }
    static KernelArg real(float v) { return {Float, 0, 0, v}; }
};

struct DispatchResult { bool ok = true; std::string error; uint64_t instructions = 0; };

/// Owns the WASM linear memory shared by all kernels, the buffers inside it, and the
/// loaded kernel instances. All access is through validated handles.
class ComputeRuntime {
public:
    explicit ComputeRuntime(uint32_t initial_pages = 16, uint32_t max_pages = 1024);

    // buffer.create / upload / download
    BufferHandle buffer_create(uint32_t bytes);
    bool buffer_destroy(BufferHandle h);
    bool buffer_upload(BufferHandle h, const void* data, uint32_t bytes, uint32_t offset = 0);
    bool buffer_download(BufferHandle h, void* out, uint32_t bytes, uint32_t offset = 0) const;
    uint32_t buffer_size(BufferHandle h) const;
    bool buffer_valid(BufferHandle h) const { return buffers_.valid(h); }

    // kernel.compile / load / dispatch
    kernel::CompiledKernel compile(const std::string& source) const { return kernel::compile(source); }
    /// Loads a compiled kernel (source is kept so save states can rebuild it).
    KernelHandle load(const kernel::CompiledKernel& k, const std::string& source, std::string& error);
    KernelHandle compile_and_load(const std::string& source, std::string& error);
    bool unload(KernelHandle h);
    DispatchResult dispatch(KernelHandle h, uint32_t grid, uint32_t block, const std::vector<KernelArg>& args);
    bool kernel_valid(KernelHandle h) const { return kernels_.valid(h); }
    const kernel::CompiledKernel* kernel_info(KernelHandle h) const;

    uint32_t live_buffers() const { return buffers_.alive(); }
    uint32_t live_kernels() const { return kernels_.alive(); }
    uint32_t bytes_in_use() const { return alloc_.bytes_used(); }
    uint32_t memory_pages() const { return memory_->pages(); }

    void serialize(BinaryWriter& w) const;
    bool deserialize(BinaryReader& r, std::string& error);

private:
    struct Buf { uint32_t offset = 0, size = 0; };
    struct Kern { std::string source; kernel::CompiledKernel compiled; std::shared_ptr<wasm::Instance> instance; };
    bool grow_to_fit(uint32_t bytes);

    std::shared_ptr<wasm::Memory> memory_;
    BufferAllocator alloc_;
    HandlePool<BufferHandle> buffers_;
    std::vector<Buf> buf_;
    HandlePool<KernelHandle> kernels_;
    std::vector<Kern> kern_;
    static constexpr uint32_t kReserved = 16;  // offset 0 is never handed out, so 0 never looks like a valid buffer
};

}  // namespace unify
