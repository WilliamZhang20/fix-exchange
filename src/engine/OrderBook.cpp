#include "OrderBook.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace engine {

OrderBook::OrderBook(std::string symbol, FillCallback on_fill)
    : symbol_(std::move(symbol)), on_fill_(std::move(on_fill)) {
    order_index_.reserve(131072);
    orders_.reserve(131072);
    levels_.reserve(1024);
}

void OrderBook::restore(Order order) {
    rest_order(std::move(order));
#ifndef NDEBUG
    assert_invariants();
#endif
}

int OrderBook::add(Order order) {
    order.leaves_qty = order.qty;
    try_match(order);
    int leaves = order.leaves_qty;
    if (order.leaves_qty > 0 && order.type == '2' && order.tif != '3' && order.tif != '4') {
        rest_order(std::move(order));
    }
#ifndef NDEBUG
    assert_invariants();
#endif
    return leaves;
}

bool OrderBook::cancel(const std::string& order_id) {
    auto idx_it = order_index_.find(order_id);
    if (idx_it == order_index_.end()) return false;
    OrderId id = idx_it->second;
    order_index_.erase(idx_it);
    bool removed = remove_resting_order(id);
#ifndef NDEBUG
    if (removed) assert_invariants();
#endif
    return removed;
}

int OrderBook::replace(const std::string& order_id, double new_price, int new_qty) {
    auto idx_it = order_index_.find(order_id);
    if (idx_it == order_index_.end()) return -1;

    OrderId id = idx_it->second;

    if (new_price == order_price(id) && new_qty > 0 && new_qty < orders_.leaves_qty[id]) {
        PriceLevel& lvl = level(orders_.level[id]);
        lvl.total_qty += new_qty - orders_.leaves_qty[id];
        orders_.qty[id] = new_qty;
        orders_.leaves_qty[id] = new_qty;
#ifndef NDEBUG
        assert_invariants();
#endif
        return new_qty;
    }

    Order order = materialize_order(id);
    order_index_.erase(idx_it);
    remove_resting_order(id);

    order.price = new_price;
    order.qty = new_qty;
    int result = add(order);
#ifndef NDEBUG
    assert_invariants();
#endif
    return result;
}

template<typename BookSide>
void OrderBook::match_against(Order& aggressor, BookSide& opposite, bool is_buy) {
    while (aggressor.leaves_qty > 0 && !opposite.empty()) {
        auto it = opposite.begin();
        double best_price = it->first;
        LevelId level_id = it->second;
        PriceLevel& lvl = level(level_id);

        bool crosses = (aggressor.type == '1') ||
                       (is_buy ? aggressor.price >= best_price
                               : aggressor.price <= best_price);
        if (!crosses) break;

        OrderId resting_id = lvl.head;
        int fill_qty = std::min(aggressor.leaves_qty, orders_.leaves_qty[resting_id]);

        aggressor.leaves_qty -= fill_qty;
        orders_.leaves_qty[resting_id] -= fill_qty;
        lvl.total_qty -= fill_qty;

        Fill taker = make_fill(aggressor, best_price, fill_qty, aggressor.leaves_qty);
        taker.arrival_ns  = aggressor.arrival_ns;
        taker.dequeue_ns  = aggressor.dequeue_ns;
        Fill maker = make_fill(resting_id, best_price, fill_qty, orders_.leaves_qty[resting_id]);
        on_fill_(maker, taker);

        if (orders_.leaves_qty[resting_id] == 0) {
            order_index_.erase(orders_.exchange_id[resting_id]);
            unlink_from_level(resting_id);
            free_order(resting_id);
        }
        erase_level_if_empty(level_id);
    }
#ifndef NDEBUG
    assert_invariants();
#endif
}

void OrderBook::try_match(Order& aggressor) {
    if (aggressor.side == '1')
        match_against(aggressor, asks_, true);
    else
        match_against(aggressor, bids_, false);
}

int OrderBook::available_to_fill(const Order& order) const {
    int total = 0;
    if (order.side == '1') {
        for (auto& kv : asks_) {
            if (order.type == '2' && kv.first > order.price) break;
            total += level(kv.second).total_qty;
        }
    } else {
        for (auto& kv : bids_) {
            if (order.type == '2' && kv.first < order.price) break;
            total += level(kv.second).total_qty;
        }
    }
    return total;
}

std::vector<BookLevel> OrderBook::getBids() const {
    std::vector<BookLevel> out;
    for (const auto& kv : bids_) {
        const PriceLevel& lvl = level(kv.second);
        if (lvl.total_qty > 0) out.push_back({kv.first, lvl.total_qty});
    }
    return out;
}

std::vector<BookLevel> OrderBook::getAsks() const {
    std::vector<BookLevel> out;
    for (const auto& kv : asks_) {
        const PriceLevel& lvl = level(kv.second);
        if (lvl.total_qty > 0) out.push_back({kv.first, lvl.total_qty});
    }
    return out;
}

std::vector<Order> OrderBook::getOrders() const {
    std::vector<Order> out;
    for (const auto& kv : bids_) {
        const PriceLevel& lvl = level(kv.second);
        for (OrderId id = lvl.head; id != kInvalidOrder; id = orders_.next[id]) {
            out.push_back(materialize_order(id));
        }
    }
    for (const auto& kv : asks_) {
        const PriceLevel& lvl = level(kv.second);
        for (OrderId id = lvl.head; id != kInvalidOrder; id = orders_.next[id]) {
            out.push_back(materialize_order(id));
        }
    }
    return out;
}

Fill OrderBook::make_fill(const Order& order, double price, int qty, int leaves) const {
    return Fill{
        symbol_ + "-" + std::to_string(++const_cast<OrderBook*>(this)->exec_seq_),
        order.clord_id,
        order.exchange_id,
        order.client_id,
        symbol_,
        order.side,
        price,
        qty,
        leaves,
        order.qty,    // order_qty
        order.type,   // order_type
        order.price,  // limit_price
    };
}

Fill OrderBook::make_fill(OrderId id, double price, int qty, int leaves) const {
    return Fill{
        symbol_ + "-" + std::to_string(++const_cast<OrderBook*>(this)->exec_seq_),
        orders_.clord_id[id],
        orders_.exchange_id[id],
        orders_.client_id[id],
        symbol_,
        order_side(id),
        price,
        qty,
        leaves,
        orders_.qty[id],
        orders_.type[id],
        order_price(id),
    };
}

Order OrderBook::materialize_order(OrderId id) const {
    Order order;
    order.clord_id = orders_.clord_id[id];
    order.exchange_id = orders_.exchange_id[id];
    order.client_id = orders_.client_id[id];
    order.symbol = symbol_;
    order.side = order_side(id);
    order.type = orders_.type[id];
    order.price = order_price(id);
    order.qty = orders_.qty[id];
    order.leaves_qty = orders_.leaves_qty[id];
    order.tif = orders_.tif[id];
    order.arrival_ns = orders_.arrival_ns[id];
    order.dequeue_ns = orders_.dequeue_ns[id];
    return order;
}

double OrderBook::order_price(OrderId id) const {
    return level(orders_.level[id]).price;
}

char OrderBook::order_side(OrderId id) const {
    return level(orders_.level[id]).side;
}

void OrderBook::rest_order(Order order) {
    LevelId level_id = get_or_create_level(order.side, order.price);
    OrderId id = allocate_order(std::move(order), level_id);
    append_to_level(level_id, id);
    level(level_id).total_qty += orders_.leaves_qty[id];
    order_index_[orders_.exchange_id[id]] = id;
}

LevelId OrderBook::get_or_create_level(char side, double price) {
    if (side == '1') {
        auto it = bids_.find(price);
        if (it != bids_.end()) return it->second;
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) return it->second;
    }

    LevelId id;
    if (!free_levels_.empty()) {
        id = free_levels_.back();
        free_levels_.pop_back();
        levels_[id] = PriceLevel{};
    } else {
        id = static_cast<LevelId>(levels_.size());
        levels_.push_back(PriceLevel{});
    }

    PriceLevel& lvl = levels_[id];
    lvl.price = price;
    lvl.side = side;
    lvl.active = true;

    if (side == '1') {
        bids_.emplace(price, id);
    } else {
        asks_.emplace(price, id);
    }
    return id;
}

OrderBook::PriceLevel& OrderBook::level(LevelId id) {
    return levels_[id];
}

const OrderBook::PriceLevel& OrderBook::level(LevelId id) const {
    return levels_[id];
}

OrderId OrderBook::allocate_order(Order order, LevelId level_id) {
    if (!free_orders_.empty()) {
        OrderId id = free_orders_.back();
        free_orders_.pop_back();
        orders_.clord_id[id] = std::move(order.clord_id);
        orders_.exchange_id[id] = std::move(order.exchange_id);
        orders_.client_id[id] = std::move(order.client_id);
        orders_.type[id] = order.type;
        orders_.qty[id] = order.qty;
        orders_.leaves_qty[id] = order.leaves_qty;
        orders_.tif[id] = order.tif;
        orders_.arrival_ns[id] = order.arrival_ns;
        orders_.dequeue_ns[id] = order.dequeue_ns;
        orders_.next[id] = kInvalidOrder;
        orders_.prev[id] = kInvalidOrder;
        orders_.level[id] = level_id;
        orders_.live[id] = 1;
        return id;
    }

    OrderId id = static_cast<OrderId>(orders_.size());
    orders_.clord_id.push_back(std::move(order.clord_id));
    orders_.exchange_id.push_back(std::move(order.exchange_id));
    orders_.client_id.push_back(std::move(order.client_id));
    orders_.type.push_back(order.type);
    orders_.qty.push_back(order.qty);
    orders_.leaves_qty.push_back(order.leaves_qty);
    orders_.tif.push_back(order.tif);
    orders_.arrival_ns.push_back(order.arrival_ns);
    orders_.dequeue_ns.push_back(order.dequeue_ns);
    orders_.next.push_back(kInvalidOrder);
    orders_.prev.push_back(kInvalidOrder);
    orders_.level.push_back(level_id);
    orders_.live.push_back(1);
    return id;
}

void OrderBook::free_order(OrderId id) {
    orders_.next[id] = kInvalidOrder;
    orders_.prev[id] = kInvalidOrder;
    orders_.level[id] = kInvalidLevel;
    orders_.live[id] = 0;
    free_orders_.push_back(id);
}

void OrderBook::append_to_level(LevelId level_id, OrderId order_id) {
    PriceLevel& lvl = level(level_id);
    orders_.level[order_id] = level_id;
    orders_.prev[order_id] = lvl.tail;
    orders_.next[order_id] = kInvalidOrder;

    if (lvl.tail != kInvalidOrder) {
        orders_.next[lvl.tail] = order_id;
    } else {
        lvl.head = order_id;
    }
    lvl.tail = order_id;
}

void OrderBook::unlink_from_level(OrderId order_id) {
    PriceLevel& lvl = level(orders_.level[order_id]);

    if (orders_.prev[order_id] != kInvalidOrder) {
        orders_.next[orders_.prev[order_id]] = orders_.next[order_id];
    } else {
        lvl.head = orders_.next[order_id];
    }

    if (orders_.next[order_id] != kInvalidOrder) {
        orders_.prev[orders_.next[order_id]] = orders_.prev[order_id];
    } else {
        lvl.tail = orders_.prev[order_id];
    }

    orders_.next[order_id] = kInvalidOrder;
    orders_.prev[order_id] = kInvalidOrder;
}

void OrderBook::erase_level_if_empty(LevelId level_id) {
    PriceLevel& lvl = level(level_id);
    if (lvl.head != kInvalidOrder) return;

    assert(lvl.tail == kInvalidOrder);
    assert(lvl.total_qty == 0);

    if (lvl.side == '1') {
        bids_.erase(lvl.price);
    } else {
        asks_.erase(lvl.price);
    }

    lvl = PriceLevel{};
    free_levels_.push_back(level_id);
}

bool OrderBook::remove_resting_order(OrderId id) {
    if (id == kInvalidOrder || id >= orders_.size() || !orders_.live[id]) {
        return false;
    }

    LevelId level_id = orders_.level[id];
    PriceLevel& lvl = level(level_id);
    lvl.total_qty -= orders_.leaves_qty[id];
    unlink_from_level(id);
    free_order(id);
    erase_level_if_empty(level_id);
    return true;
}

#ifndef NDEBUG
void OrderBook::assert_invariants() const {
    std::vector<bool> seen_orders(orders_.size(), false);

    for (const auto& kv : bids_) {
        LevelId level_id = kv.second;
        assert(level_id < levels_.size());
        const PriceLevel& lvl = levels_[level_id];
        assert(lvl.active);
        assert(lvl.side == '1');
        assert(lvl.price == kv.first);
    }

    for (const auto& kv : asks_) {
        LevelId level_id = kv.second;
        assert(level_id < levels_.size());
        const PriceLevel& lvl = levels_[level_id];
        assert(lvl.active);
        assert(lvl.side == '2');
        assert(lvl.price == kv.first);
    }

    for (LevelId level_id = 0; level_id < levels_.size(); ++level_id) {
        const PriceLevel& lvl = levels_[level_id];
        if (!lvl.active) continue;

        assert((lvl.head == kInvalidOrder) == (lvl.tail == kInvalidOrder));
        assert((lvl.head == kInvalidOrder) == (lvl.total_qty == 0));

        int total = 0;
        OrderId prev = kInvalidOrder;
        OrderId last = kInvalidOrder;
        for (OrderId id = lvl.head; id != kInvalidOrder; id = orders_.next[id]) {
            assert(id < orders_.size());
            assert(orders_.live[id]);
            assert(orders_.level[id] == level_id);
            assert(orders_.prev[id] == prev);
            if (orders_.next[id] != kInvalidOrder) {
                assert(orders_.next[id] < orders_.size());
                assert(orders_.prev[orders_.next[id]] == id);
            }
            auto idx_it = order_index_.find(orders_.exchange_id[id]);
            assert(idx_it != order_index_.end());
            assert(idx_it->second == id);
            assert(!seen_orders[id]);
            seen_orders[id] = true;
            total += orders_.leaves_qty[id];
            prev = id;
            last = id;
        }

        assert(lvl.tail == last);
        assert(lvl.total_qty == total);
    }

    for (OrderId id = 0; id < orders_.size(); ++id) {
        if (!orders_.live[id]) continue;
        assert(seen_orders[id]);
        assert(orders_.level[id] < levels_.size());
        assert(levels_[orders_.level[id]].active);
        auto idx_it = order_index_.find(orders_.exchange_id[id]);
        assert(idx_it != order_index_.end());
        assert(idx_it->second == id);
    }
}
#endif

} // namespace engine
