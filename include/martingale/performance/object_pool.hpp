#ifndef MARTINGALE_PERFORMANCE_OBJECT_POOL_HPP
#define MARTINGALE_PERFORMANCE_OBJECT_POOL_HPP

#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace martingale {

/// Fixed-capacity, pre-allocated contiguous object pool with O(1) LIFO reuse
/// to eliminate dynamic heap allocations from latency-critical hot paths.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        if (capacity == 0U) {
            throw std::invalid_argument("ObjectPool capacity must be positive");
        }
        free_list_.reserve(capacity);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) noexcept = default;
    ObjectPool& operator=(ObjectPool&&) noexcept = default;
    ~ObjectPool() = default;

    /// Acquires a pre-allocated slot in O(1) time without heap allocation.
    /// Reuses the most recently released slot (LIFO) for L1 cache warmth.
    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        T* slot = nullptr;
        if (!free_list_.empty()) {
            slot = free_list_.back();
            free_list_.pop_back();
        } else if (high_water_mark_ < storage_.size()) {
            slot = &storage_[high_water_mark_++];
        } else {
            throw std::bad_alloc();
        }

        *slot = T(std::forward<Args>(args)...);
        ++active_count_;
        return slot;
    }

    /// Returns an acquired slot to the LIFO free list in O(1) time.
    void release(T* ptr) noexcept {
        if (ptr == nullptr) {
            return;
        }
        free_list_.push_back(ptr);
        --active_count_;
    }

    /// Resets the pool in O(1) time without releasing backing storage.
    void reset() noexcept {
        free_list_.clear();
        high_water_mark_ = 0U;
        active_count_ = 0U;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t available_count() const noexcept {
        return storage_.size() - active_count_;
    }

private:
    std::vector<T> storage_;
    std::vector<T*> free_list_{};
    std::size_t high_water_mark_{0};
    std::size_t active_count_{0};
};

}  // namespace martingale

#endif  // MARTINGALE_PERFORMANCE_OBJECT_POOL_HPP
