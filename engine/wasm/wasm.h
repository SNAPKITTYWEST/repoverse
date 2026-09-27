#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace unify::wasm {

enum class ValType : uint8_t { I32 = 0x7F, I64 = 0x7E, F32 = 0x7D, F64 = 0x7C };

struct FuncType {
    std::vector<ValType> params, results;
    bool operator==(const FuncType& o) const { return params == o.params && results == o.results; }
};

/// A WASM value: raw bits plus type. f32/f64 are stored as bit patterns (exact).
struct Value {
    ValType type = ValType::I32;
    uint64_t bits = 0;
    static Value i32(int32_t v) { return {ValType::I32, uint64_t(uint32_t(v))}; }
    static Value i64(int64_t v) { return {ValType::I64, uint64_t(v)}; }
    static Value f32(float v);
    static Value f64(double v);
    int32_t as_i32() const { return int32_t(uint32_t(bits)); }
    int64_t as_i64() const { return int64_t(bits); }
    float as_f32() const;
    double as_f64() const;
};

struct Import { std::string module, name; uint8_t kind; uint32_t type_index = 0; uint32_t min_pages = 0, max_pages = 0; bool has_max = false; };
struct Export { std::string name; uint8_t kind; uint32_t index; };
struct Global { ValType type; bool mut; Value init; };
struct DataSegment { uint32_t offset; std::vector<uint8_t> bytes; };

struct Function {
    uint32_t type_index = 0;
    std::vector<ValType> locals;   // declared locals (excluding params)
    std::vector<uint8_t> code;     // body bytes (after the locals header)
    // Side tables built at decode time: position of each block/loop/if -> matching else/end.
    std::map<uint32_t, uint32_t> end_of;
    std::map<uint32_t, uint32_t> else_of;
};

/// Decoded, validated-for-structure module.
struct Module {
    std::vector<FuncType> types;
    std::vector<Import> imports;
    uint32_t imported_funcs = 0;
    std::vector<Function> functions;   // defined functions (index offset by imported_funcs)
    bool has_memory = false, memory_imported = false;
    uint32_t memory_min = 0, memory_max = 0;
    bool memory_has_max = false;
    std::vector<Global> globals;
    std::vector<Export> exports;
    std::vector<DataSegment> data;
    std::vector<uint32_t> table;       // funcref table (for call_indirect), flattened from elem segments
};

struct DecodeError { std::string message; };
/// Decodes a binary module. Returns false and fills `error` on malformed input.
bool decode(const std::vector<uint8_t>& bytes, Module& out, std::string& error);

/// Linear memory. Owned outside instances so several modules can share one memory.
struct Memory {
    std::vector<uint8_t> bytes;
    uint32_t max_pages = 65536;
    static constexpr uint32_t kPage = 65536;
    uint32_t pages() const { return uint32_t(bytes.size() / kPage); }
    /// Returns old page count, or -1 on failure.
    int32_t grow(uint32_t delta) {
        uint32_t old = pages();
        if (uint64_t(old) + delta > max_pages) return -1;
        bytes.resize(size_t(old + delta) * kPage, 0);
        return int32_t(old);
    }
};

using HostFunc = std::function<bool(const std::vector<Value>& args, std::vector<Value>& results, std::string& trap)>;

struct ExecResult {
    bool ok = true;
    std::string trap;
    std::vector<Value> results;
    uint64_t instructions = 0;
};

/// An instantiated module: globals, memory binding, host imports, and an interpreter.
class Instance {
public:
    /// `shared_memory` satisfies an imported memory; required if the module imports one.
    bool instantiate(std::shared_ptr<const Module> module, std::shared_ptr<Memory> shared_memory,
                     const std::map<std::string, HostFunc>& host_funcs, std::string& error);
    ExecResult call(const std::string& export_name, const std::vector<Value>& args);
    ExecResult call_index(uint32_t func_index, const std::vector<Value>& args);
    std::shared_ptr<Memory> memory() const { return memory_; }
    const Module& module() const { return *module_; }
    bool has_export(const std::string& name) const;

    uint64_t fuel = 500'000'000;      // instruction budget per call: deterministic runaway guard
    uint32_t max_call_depth = 512;

private:
    bool run(uint32_t func_index, std::vector<uint64_t>& stack, std::string& trap, uint64_t& executed, uint32_t depth);
    const FuncType& type_of(uint32_t func_index) const;
    std::shared_ptr<const Module> module_;
    std::shared_ptr<Memory> memory_;
    std::vector<Value> globals_;
    std::vector<HostFunc> host_;     // one per imported function
};

// --------------------------------------------------------------------------- encoding helpers
void write_uleb(std::vector<uint8_t>& out, uint64_t v);
void write_sleb(std::vector<uint8_t>& out, int64_t v);

}  // namespace unify::wasm
