#include "compute.h"
#include "../core/core.h"
#include "../serialization/stream.h"
#include <cstring>

namespace unify {

ComputeRuntime::ComputeRuntime(uint32_t initial_pages, uint32_t max_pages) : memory_(std::make_shared<wasm::Memory>()) {
    memory_->max_pages = max_pages;
    memory_->bytes.assign(size_t(initial_pages) * wasm::Memory::kPage, 0);
    alloc_.reset(kReserved, uint32_t(memory_->bytes.size()) - kReserved);
}

bool ComputeRuntime::grow_to_fit(uint32_t bytes) {
    uint32_t need_pages = (bytes + wasm::Memory::kPage - 1) / wasm::Memory::kPage + 1;
    if (memory_->grow(need_pages) < 0) return false;
    alloc_.extend(uint32_t(memory_->bytes.size()));
    return true;
}

BufferHandle ComputeRuntime::buffer_create(uint32_t bytes) {
    uint32_t off = alloc_.allocate(bytes, 16);
    if (off == BufferAllocator::kFail) {
        if (!grow_to_fit(bytes)) { UNIFY_LOG_ERROR("COMPUTE", "out of kernel memory (%u bytes requested)", bytes); return {}; }
        off = alloc_.allocate(bytes, 16);
        if (off == BufferAllocator::kFail) return {};
    }
    std::memset(memory_->bytes.data() + off, 0, bytes);
    BufferHandle h = buffers_.allocate();
    if (h.index() >= buf_.size()) buf_.resize(h.index() + 1);
    buf_[h.index()] = {off, bytes};
    return h;
}

bool ComputeRuntime::buffer_destroy(BufferHandle h) {
    if (!buffers_.valid(h)) return false;
    alloc_.free(buf_[h.index()].offset);
    buf_[h.index()] = {};
    return buffers_.release(h);
}

bool ComputeRuntime::buffer_upload(BufferHandle h, const void* data, uint32_t bytes, uint32_t offset) {
    if (!buffers_.valid(h)) return false;
    const Buf& b = buf_[h.index()];
    if (uint64_t(offset) + bytes > b.size) return false;
    std::memcpy(memory_->bytes.data() + b.offset + offset, data, bytes);
    return true;
}

bool ComputeRuntime::buffer_download(BufferHandle h, void* out, uint32_t bytes, uint32_t offset) const {
    if (!buffers_.valid(h)) return false;
    const Buf& b = buf_[h.index()];
    if (uint64_t(offset) + bytes > b.size) return false;
    std::memcpy(out, memory_->bytes.data() + b.offset + offset, bytes);
    return true;
}

uint32_t ComputeRuntime::buffer_size(BufferHandle h) const { return buffers_.valid(h) ? buf_[h.index()].size : 0; }

KernelHandle ComputeRuntime::load(const kernel::CompiledKernel& k, const std::string& source, std::string& error) {
    if (!k.ok) { error = k.error; return {}; }
    auto module = std::make_shared<wasm::Module>();
    if (!wasm::decode(k.wasm, *module, error)) return {};
    auto inst = std::make_shared<wasm::Instance>();
    if (!inst->instantiate(module, memory_, {}, error)) return {};
    KernelHandle h = kernels_.allocate();
    if (h.index() >= kern_.size()) kern_.resize(h.index() + 1);
    kern_[h.index()] = {source, k, inst};
    return h;
}

KernelHandle ComputeRuntime::compile_and_load(const std::string& source, std::string& error) {
    kernel::CompiledKernel k = compile(source);
    if (!k.ok) { error = k.error; return {}; }
    return load(k, source, error);
}

bool ComputeRuntime::unload(KernelHandle h) {
    if (!kernels_.valid(h)) return false;
    kern_[h.index()] = {};
    return kernels_.release(h);
}

const kernel::CompiledKernel* ComputeRuntime::kernel_info(KernelHandle h) const {
    return kernels_.valid(h) ? &kern_[h.index()].compiled : nullptr;
}

DispatchResult ComputeRuntime::dispatch(KernelHandle h, uint32_t grid, uint32_t block, const std::vector<KernelArg>& args) {
    DispatchResult r;
    if (!kernels_.valid(h)) { r.ok = false; r.error = "invalid kernel handle"; return r; }
    Kern& k = kern_[h.index()];
    const auto& params = k.compiled.params;
    if (args.size() != params.size()) {
        r.ok = false;
        r.error = k.compiled.name + " expects " + std::to_string(params.size()) + " arguments, got " + std::to_string(args.size());
        return r;
    }
    std::vector<wasm::Value> wargs = {wasm::Value::i32(int32_t(grid)), wasm::Value::i32(int32_t(block))};
    for (size_t i = 0; i < args.size(); i++) {
        const KernelArg& a = args[i];
        kernel::Type t = params[i].type;
        bool is_ptr = t == kernel::Type::IntPtr || t == kernel::Type::FloatPtr;
        if (is_ptr) {
            if (a.kind != KernelArg::Buffer || !buffers_.valid(BufferHandle{a.buffer})) {
                r.ok = false; r.error = "argument '" + params[i].name + "' must be a valid buffer"; return r;
            }
            wargs.push_back(wasm::Value::i32(int32_t(buf_[BufferHandle{a.buffer}.index()].offset)));
        } else if (t == kernel::Type::Int) {
            wargs.push_back(wasm::Value::i32(a.kind == KernelArg::Float ? int32_t(a.f) : a.i));
        } else {
            wargs.push_back(wasm::Value::f32(a.kind == KernelArg::Int ? float(a.i) : a.f));
        }
    }
    wasm::ExecResult x = k.instance->call(k.compiled.name + "__dispatch", wargs);
    r.ok = x.ok;
    r.error = x.trap;
    r.instructions = x.instructions;
    return r;
}

void ComputeRuntime::serialize(BinaryWriter& w) const {
    w.u32(memory_->pages());
    auto bs = buffers_.save();
    w.u32(uint32_t(bs.gens.size()));
    for (size_t i = 0; i < bs.gens.size(); i++) {
        w.u8(bs.gens[i]); w.u8(bs.live[i]);
        if (bs.live[i]) {
            const Buf& b = buf_[i];
            w.u32(b.offset); w.u32(b.size);
            w.bytes(memory_->bytes.data() + b.offset, b.size);  // only live buffer contents, not the whole memory
        }
    }
    w.u32(uint32_t(bs.free.size()));
    for (uint32_t f : bs.free) w.u32(f);
    auto ks = kernels_.save();
    w.u32(uint32_t(ks.gens.size()));
    for (size_t i = 0; i < ks.gens.size(); i++) {
        w.u8(ks.gens[i]); w.u8(ks.live[i]);
        if (ks.live[i]) w.str(kern_[i].source);  // kernels are rebuilt from source on load
    }
    w.u32(uint32_t(ks.free.size()));
    for (uint32_t f : ks.free) w.u32(f);
}

bool ComputeRuntime::deserialize(BinaryReader& r, std::string& error) {
    uint32_t pages = r.u32();
    memory_->bytes.assign(size_t(pages) * wasm::Memory::kPage, 0);
    HandlePool<BufferHandle>::State bs;
    uint32_t n = r.u32();
    bs.gens.resize(n); bs.live.resize(n);
    buf_.assign(n, Buf{});
    std::map<uint32_t, uint32_t> used;
    for (uint32_t i = 0; i < n; i++) {
        bs.gens[i] = r.u8(); bs.live[i] = r.u8();
        if (!bs.live[i]) continue;
        bs.alive++;
        Buf b{r.u32(), r.u32()};
        if (uint64_t(b.offset) + b.size > memory_->bytes.size()) { error = "buffer out of range in save state"; return false; }
        r.bytes(memory_->bytes.data() + b.offset, b.size);
        buf_[i] = b;
        used[b.offset] = b.size;
    }
    bs.free.resize(r.u32());
    for (auto& f : bs.free) f = r.u32();
    buffers_.load(bs);
    alloc_.restore(used, kReserved, uint32_t(memory_->bytes.size()));

    HandlePool<KernelHandle>::State ks;
    uint32_t nk = r.u32();
    ks.gens.resize(nk); ks.live.resize(nk);
    kern_.assign(nk, Kern{});
    for (uint32_t i = 0; i < nk; i++) {
        ks.gens[i] = r.u8(); ks.live[i] = r.u8();
        if (!ks.live[i]) continue;
        ks.alive++;
        std::string src = r.str();
        kernel::CompiledKernel k = compile(src);
        if (!k.ok) { error = "kernel failed to recompile: " + k.error; return false; }
        auto module = std::make_shared<wasm::Module>();
        if (!wasm::decode(k.wasm, *module, error)) return false;
        auto inst = std::make_shared<wasm::Instance>();
        if (!inst->instantiate(module, memory_, {}, error)) return false;
        kern_[i] = {src, k, inst};
    }
    ks.free.resize(r.u32());
    for (auto& f : ks.free) f = r.u32();
    kernels_.load(ks);
    return true;
}

}  // namespace unify
