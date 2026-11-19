#ifndef MARTINGALE_BOOK_FLAT_ORDER_BOOK_HPP
#define MARTINGALE_BOOK_FLAT_ORDER_BOOK_HPP

#include <martingale/book/order_book.hpp>
#include <martingale/core/order.hpp>
#include <martingale/core/types.hpp>
#include <martingale/performance/object_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace martingale {

/// Cache-line-aligned intrusive order node combining a 48-byte Order payload
/// with two 8-byte FIFO doubly-linked pointers (64 bytes total).
struct alignas(kCacheLineSize) IntrusiveOrderNode {
    Order order{};
    IntrusiveOrderNode* prev{nullptr};
    IntrusiveOrderNode* next{nullptr};

    constexpr IntrusiveOrderNode() noexcept = default;
    explicit constexpr IntrusiveOrderNode(const Order& ord) noexcept : order(ord) {}
};

static_assert(sizeof(IntrusiveOrderNode) == kCacheLineSize);
static_assert(alignof(IntrusiveOrderNode) == kCacheLineSize);

/// Contiguous-node FIFO queue at a single discrete tick price using intrusive
/// links—eliminating per-order std::list node heap allocations.
class IntrusivePriceLevel {
public:
    explicit IntrusivePriceLevel(Price level_price) noexcept : price_(level_price) {}

    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
    [[nodiscard]] bool empty() const noexcept { return order_count_ == 0U; }

    [[nodiscard]] const Order& front() const {
        if (head_ == nullptr) {
            throw std::out_of_range("Cannot inspect front of an empty IntrusivePriceLevel");
        }
        return head_->order;
    }

    void push_back(IntrusiveOrderNode* node) {
        if (node == nullptr || node->order.price != price_ || !node->order.is_active() ||
            node->order.remaining_quantity == 0U) {
            throw std::invalid_argument("Invalid node for IntrusivePriceLevel");
        }
        node->prev = tail_;
        node->next = nullptr;
        if (tail_ != nullptr) {
            tail_->next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        total_quantity_ += node->order.remaining_quantity;
        ++order_count_;
    }

    void fill_front(Quantity exec_qty) {
        if (head_ == nullptr) {
            throw std::out_of_range("Cannot fill an empty IntrusivePriceLevel");
        }
        head_->order.fill(exec_qty);
        total_quantity_ -= exec_qty;
    }

    void unlink(IntrusiveOrderNode* node) noexcept {
        if (node == nullptr) {
            return;
        }
        total_quantity_ -= node->order.remaining_quantity;
        --order_count_;

        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            head_ = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else {
            tail_ = node->prev;
        }

        node->prev = nullptr;
        node->next = nullptr;
    }

    IntrusiveOrderNode* pop_front() {
        if (head_ == nullptr) {
            throw std::out_of_range("Cannot pop front of an empty IntrusivePriceLevel");
        }
        IntrusiveOrderNode* front_node = head_;
        unlink(front_node);
        return front_node;
    }

private:
    Price price_{kInvalidPrice};
    Quantity total_quantity_{0};
    std::size_t order_count_{0};
    IntrusiveOrderNode* head_{nullptr};
    IntrusiveOrderNode* tail_{nullptr};
};

/// High-performance Limit Order Book combining pre-allocated ObjectPool<IntrusiveOrderNode>,
/// intrusive doubly-linked FIFO queues, contiguous sorted price-level vectors (with BBO at back()
/// for O(1) top-of-book pop_back), and flat array + sparse fallback O(1) order lookup.
class FlatOrderBook {
public:
    static constexpr std::size_t kDefaultPoolCapacity = 16384U;

    explicit FlatOrderBook(InstrumentId instrument_id,
                           std::size_t pool_capacity = kDefaultPoolCapacity)
        : instrument_id_(instrument_id),
          pool_(pool_capacity),
          direct_lookup_(pool_capacity, nullptr) {
        bids_.reserve(256U);
        asks_.reserve(256U);
    }

    [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }
    [[nodiscard]] bool empty() const noexcept { return pool_.active_count() == 0U; }
    [[nodiscard]] std::size_t active_order_count() const noexcept { return pool_.active_count(); }
    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.back().price());
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.back().price());
    }

    [[nodiscard]] Quantity best_bid_quantity() const noexcept {
        return bids_.empty() ? 0U : bids_.back().total_quantity();
    }

    [[nodiscard]] Quantity best_ask_quantity() const noexcept {
        return asks_.empty() ? 0U : asks_.back().total_quantity();
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return std::nullopt;
        }
        return asks_.back().price() - bids_.back().price();
    }

    [[nodiscard]] std::optional<Price> mid_price_ticks() const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return std::nullopt;
        }
        const Price bid = bids_.back().price();
        const Price ask = asks_.back().price();
        return bid + (ask - bid) / 2;
    }

    [[nodiscard]] Quantity quantity_at_price(Side side, Price price) const noexcept {
        return side == Side::Buy ? quantity_at_price_impl(bids_, price, true)
                                 : quantity_at_price_impl(asks_, price, false);
    }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept {
        const IntrusiveOrderNode* node = lookup_node(id);
        return (node != nullptr) ? &node->order : nullptr;
    }

    [[nodiscard]] const Order* best_order(Side side) const noexcept {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        return levels.empty() ? nullptr : &levels.back().front();
    }

    [[nodiscard]] bool add_order(const Order& order) {
        if (order.instrument_id != instrument_id_) {
            throw std::invalid_argument("Order instrument_id does not match FlatOrderBook");
        }
        if (order.type != OrderType::Limit) {
            throw std::invalid_argument("Only Limit orders can rest on the FlatOrderBook");
        }
        if (order.price <= 0) {
            throw std::invalid_argument("Limit order price must be positive");
        }
        if (!is_valid_side(order.side) || !order.is_active() || order.remaining_quantity == 0U) {
            throw std::invalid_argument("Invalid or inactive order");
        }
        if (lookup_node(order.id) != nullptr) {
            return false;
        }

        IntrusiveOrderNode* node = pool_.acquire(order);
        if (order.side == Side::Buy) {
            insert_into_levels(bids_, node, true);
        } else {
            insert_into_levels(asks_, node, false);
        }
        store_node(order.id, node);
        return true;
    }

    [[nodiscard]] std::optional<Order> cancel_order(OrderId id) {
        IntrusiveOrderNode* node = lookup_node(id);
        if (node == nullptr) {
            return std::nullopt;
        }

        erase_node_entry(id);
        if (node->order.side == Side::Buy) {
            remove_from_levels(bids_, node, true);
        } else {
            remove_from_levels(asks_, node, false);
        }

        Order removed = node->order;
        pool_.release(node);
        removed.cancel();
        return removed;
    }

    void fill_best_order(Side side, Quantity exec_qty) {
        if (side == Side::Buy) {
            fill_best_impl(bids_, exec_qty);
        } else {
            fill_best_impl(asks_, exec_qty);
        }
    }

    [[nodiscard]] std::vector<DepthLevel> get_depth(Side side, std::size_t max_levels) const {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        std::vector<DepthLevel> depth;
        const std::size_t count = std::min(levels.size(), max_levels);
        depth.reserve(count);

        for (auto it = levels.rbegin(); it != levels.rend() && depth.size() < max_levels; ++it) {
            depth.push_back(DepthLevel{it->price(), it->total_quantity(), it->order_count()});
        }
        return depth;
    }

private:
    using LevelVector = std::vector<IntrusivePriceLevel>;

    [[nodiscard]] IntrusiveOrderNode* lookup_node(OrderId id) const noexcept {
        if (id < direct_lookup_.size()) {
            return direct_lookup_[id];
        }
        const auto it = sparse_lookup_.find(id);
        return (it == sparse_lookup_.end()) ? nullptr : it->second;
    }

    void store_node(OrderId id, IntrusiveOrderNode* node) {
        if (id < direct_lookup_.size()) {
            direct_lookup_[id] = node;
        } else {
            sparse_lookup_.emplace(id, node);
        }
    }

    void erase_node_entry(OrderId id) noexcept {
        if (id < direct_lookup_.size()) {
            direct_lookup_[id] = nullptr;
        } else {
            sparse_lookup_.erase(id);
        }
    }

    [[nodiscard]] static LevelVector::iterator find_level(LevelVector& levels, Price price,
                                                          bool ascending) noexcept {
        if (ascending) {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const IntrusivePriceLevel& lvl, Price p) { return lvl.price() < p; });
        }
        return std::lower_bound(
            levels.begin(), levels.end(), price,
            [](const IntrusivePriceLevel& lvl, Price p) { return lvl.price() > p; });
    }

    [[nodiscard]] static LevelVector::const_iterator find_level(const LevelVector& levels,
                                                                Price price,
                                                                bool ascending) noexcept {
        if (ascending) {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const IntrusivePriceLevel& lvl, Price p) { return lvl.price() < p; });
        }
        return std::lower_bound(
            levels.begin(), levels.end(), price,
            [](const IntrusivePriceLevel& lvl, Price p) { return lvl.price() > p; });
    }

    [[nodiscard]] static Quantity quantity_at_price_impl(const LevelVector& levels, Price price,
                                                         bool ascending) noexcept {
        const auto it = find_level(levels, price, ascending);
        if (it == levels.end() || it->price() != price) {
            return 0U;
        }
        return it->total_quantity();
    }

    static void insert_into_levels(LevelVector& levels, IntrusiveOrderNode* node, bool ascending) {
        const Price price = node->order.price;
        auto it = find_level(levels, price, ascending);
        if (it == levels.end() || it->price() != price) {
            it = levels.emplace(it, price);
        }
        it->push_back(node);
    }

    static void remove_from_levels(LevelVector& levels, IntrusiveOrderNode* node, bool ascending) {
        const Price price = node->order.price;
        auto it = find_level(levels, price, ascending);
        if (it == levels.end() || it->price() != price) {
            throw std::logic_error("FlatOrderBook price-level index invariant violated");
        }
        it->unlink(node);
        if (it->empty()) {
            if (it == std::prev(levels.end())) {
                levels.pop_back();
            } else {
                levels.erase(it);
            }
        }
    }

    void fill_best_impl(LevelVector& levels, Quantity exec_qty) {
        if (levels.empty()) {
            throw std::out_of_range("Cannot fill best order on empty side of FlatOrderBook");
        }
        IntrusivePriceLevel& level = levels.back();
        const OrderId best_id = level.front().id;

        level.fill_front(exec_qty);

        if (level.front().is_filled()) {
            IntrusiveOrderNode* popped = level.pop_front();
            erase_node_entry(best_id);
            pool_.release(popped);
            if (level.empty()) {
                levels.pop_back();
            }
        }
    }

    InstrumentId instrument_id_{kInvalidInstrumentId};
    ObjectPool<IntrusiveOrderNode> pool_;
    LevelVector bids_{};
    LevelVector asks_{};
    std::vector<IntrusiveOrderNode*> direct_lookup_;
    std::unordered_map<OrderId, IntrusiveOrderNode*> sparse_lookup_{};
};

}  // namespace martingale

#endif  // MARTINGALE_BOOK_FLAT_ORDER_BOOK_HPP
