#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <new>
#include <utility>
#include <atomic>
#include <string>
#include <map>

namespace unify {

inline size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/// Process-wide count of global operator new calls. Lets tests assert that the
/// steady-state simulation tick performs no heap allocation.
struct HeapStats {
    static std::atomic<uint64_t> allocations;
    static std::atomic<uint64_t> bytes;
};

// ---------------------------------------------------------------------------------------
/// Linear per-frame scratch memory. Reset once per frame; allocation is a pointer bump.
/// Overflow is a hard failure reported via nullptr + counter, never a silent heap fallback.
class FrameAllocator {
public:
    explicit FrameAllocator(size_t capacity) : buffer_(capacity) {}
    void* allocate(size_t size, size_t align = alignof(std::max_align_t)) {
        // Align the address, not the offset: the buffer itself is only malloc-aligned.
        uintptr_t base = reinterpret_cast<uintptr_t>(buffer_.data());
        size_t start = size_t(align_up(base + offset_, align) - base);
        if (start + size > buffer_.size()) { overflows_++; return nullptr; }
        offset_ = start + size;
        if (offset_ > high_water_) high_water_ = offset_;
        return buffer_.data() + start;
    }
    template <typename T> T* alloc_array(size_t n) {
        void* p = allocate(sizeof(T) * n, alignof(T));
        return p ? new (p) T[n]() : nullptr;
    }
    void reset() { offset_ = 0; }
    size_t used() const { return offset_; }
    size_t capacity() const { return buffer_.size(); }
    size_t high_water() const { return high_water_; }
    uint64_t overflows() const { return overflows_; }
private:
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0, high_water_ = 0;
    uint64_t overflows_ = 0;
};

// ---------------------------------------------------------------------------------------
/// Fixed-size block allocator over pre-reserved pages; O(1) alloc/free via intrusive free list.
class PoolAllocator {
public:
    PoolAllocator(size_t block_size, size_t blocks_per_page)
        : block_(align_up(std::max(block_size, sizeof(void*)), alignof(std::max_align_t))), per_page_(blocks_per_page) {}
    ~PoolAllocator() { for (auto* p : pages_) ::operator delete(p); }
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* allocate() {
        if (!free_) grow();
        void* p = free_;
        free_ = *static_cast<void**>(free_);
        in_use_++;
        return p;
    }
    void free(void* p) {
        if (!p) return;
        *static_cast<void**>(p) = free_;
        free_ = p;
        in_use_--;
    }
    size_t in_use() const { return in_use_; }
    size_t capacity() const { return pages_.size() * per_page_; }
    size_t block_size() const { return block_; }
    void reserve_blocks(size_t n) { while (capacity() < n) grow(); }
private:
    void grow() {
        auto* page = static_cast<uint8_t*>(::operator new(block_ * per_page_));
        pages_.push_back(page);
        for (size_t i = per_page_; i-- > 0;) {
            void* b = page + i * block_;
            *static_cast<void**>(b) = free_;
            free_ = b;
        }
    }
    size_t block_, per_page_;
    std::vector<uint8_t*> pages_;
    void* free_ = nullptr;
    size_t in_use_ = 0;
};

/// Typed object pool on top of PoolAllocator. Constructs in place; destroy() runs the destructor.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t per_page = 256) : pool_(sizeof(T), per_page) {}
    template <typename... Args> T* create(Args&&... args) { return new (pool_.allocate()) T(std::forward<Args>(args)...); }
    void destroy(T* obj) { if (obj) { obj->~T(); pool_.free(obj); } }
    size_t live() const { return pool_.in_use(); }
    void reserve(size_t n) { pool_.reserve_blocks(n); }
private:
    PoolAllocator pool_;
};

/// STL allocator over a per-type PoolAllocator. Node-based containers (std::map contacts)
/// recycle nodes through the pool instead of hitting the heap on every insert/erase.
template <typename T>
class PoolStdAllocator {
public:
    using value_type = T;
    PoolStdAllocator() noexcept = default;
    template <typename U> PoolStdAllocator(const PoolStdAllocator<U>&) noexcept {}
    T* allocate(size_t n) {
        if (n == 1) return static_cast<T*>(pool().allocate());
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, size_t n) noexcept {
        if (n == 1) pool().free(p);
        else ::operator delete(p);
    }
    static PoolAllocator& pool() { static PoolAllocator p(sizeof(T), 512); return p; }
    template <typename U> bool operator==(const PoolStdAllocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const PoolStdAllocator<U>&) const noexcept { return false; }
};

// ---------------------------------------------------------------------------------------
/// Accounts asset memory per category against a budget. Asset loaders allocate through it
/// so leaks after scene unload are observable (bytes return to baseline).
class AssetAllocator {
public:
    explicit AssetAllocator(size_t budget) : budget_(budget) {}
    uint8_t* allocate(size_t bytes, const char* category) {
        if (used_ + bytes > budget_) { failures_++; return nullptr; }
        auto* p = static_cast<uint8_t*>(::operator new(bytes, std::nothrow));
        if (!p) { failures_++; return nullptr; }
        used_ += bytes;
        by_category_[category] += bytes;
        sizes_[p] = {bytes, category};
        return p;
    }
    void free(uint8_t* p) {
        auto it = sizes_.find(p);
        if (it == sizes_.end()) return;
        used_ -= it->second.first;
        by_category_[it->second.second] -= it->second.first;
        sizes_.erase(it);
        ::operator delete(p);
    }
    size_t used() const { return used_; }
    size_t budget() const { return budget_; }
    uint64_t failures() const { return failures_; }
    size_t used_by(const std::string& category) const {
        auto it = by_category_.find(category);
        return it == by_category_.end() ? 0 : it->second;
    }
    size_t live_allocations() const { return sizes_.size(); }
private:
    size_t budget_, used_ = 0;
    uint64_t failures_ = 0;
    std::map<std::string, size_t> by_category_;
    std::map<uint8_t*, std::pair<size_t, std::string>> sizes_;
};

// ---------------------------------------------------------------------------------------
/// Sub-allocates ranges of an externally owned linear memory (e.g. a WASM instance's memory).
/// First-fit over an ordered free list with coalescing. Offsets, not pointers, are returned,
/// because the backing memory may grow and move.
class BufferAllocator {
public:
    explicit BufferAllocator(uint32_t base = 0, uint32_t size = 0) { reset(base, size); }
    void reset(uint32_t base, uint32_t size) {
        free_.clear();
        used_.clear();
        if (size) free_[base] = size;
        base_ = base;
        end_ = base + size;
    }
    /// Extends the managed range (after the backing memory grew).
    void extend(uint32_t new_end) {
        if (new_end <= end_) return;
        insert_free(end_, new_end - end_);
        end_ = new_end;
    }
    static constexpr uint32_t kFail = 0xFFFFFFFFu;
    uint32_t allocate(uint32_t size, uint32_t align = 16) {
        if (size == 0) size = 1;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint32_t start = uint32_t(align_up(it->first, align));
            uint32_t pad = start - it->first;
            if (it->second < pad + size) continue;
            uint32_t block_start = it->first, block_size = it->second;
            free_.erase(it);
            if (pad) free_[block_start] = pad;
            uint32_t tail = block_size - pad - size;
            if (tail) free_[start + size] = tail;
            used_[start] = size;
            return start;
        }
        return kFail;
    }
    bool free(uint32_t offset) {
        auto it = used_.find(offset);
        if (it == used_.end()) return false;
        uint32_t size = it->second;
        used_.erase(it);
        insert_free(offset, size);
        return true;
    }
    uint32_t size_of(uint32_t offset) const { auto it = used_.find(offset); return it == used_.end() ? 0 : it->second; }
    size_t live() const { return used_.size(); }
    uint32_t bytes_used() const { uint32_t n = 0; for (auto& kv : used_) n += kv.second; return n; }
    uint32_t largest_free() const { uint32_t m = 0; for (auto& kv : free_) m = std::max(m, kv.second); return m; }
    const std::map<uint32_t, uint32_t>& used_ranges() const { return used_; }
    void restore(const std::map<uint32_t, uint32_t>& used, uint32_t base, uint32_t end) {
        reset(base, end - base);
        free_.clear();
        used_ = used;
        uint32_t cursor = base;
        for (auto& kv : used_) { if (kv.first > cursor) free_[cursor] = kv.first - cursor; cursor = kv.first + kv.second; }
        if (cursor < end) free_[cursor] = end - cursor;
    }
private:
    void insert_free(uint32_t offset, uint32_t size) {
        auto next = free_.lower_bound(offset);
        if (next != free_.end() && offset + size == next->first) { size += next->second; next = free_.erase(next); }
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) { prev->second += size; return; }
        }
        free_[offset] = size;
    }
    std::map<uint32_t, uint32_t> free_, used_;
    uint32_t base_ = 0, end_ = 0;
};

}  // namespace unify
