#pragma once
#include "Order.h"
#include <absl/container/btree_map.h>
#include <absl/container/flat_hash_map.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace engine {

using FillCallback = std::function<void(const Fill& maker, const Fill& taker)>;
using OrderId = std::uint32_t;
using LevelId = std::uint32_t;
constexpr OrderId kInvalidOrder = std::numeric_limits<OrderId>::max();
constexpr LevelId kInvalidLevel = std::numeric_limits<LevelId>::max();

class OrderBook {
public:
    explicit OrderBook(std::string symbol, FillCallback on_fill);

    int  add(Order order);   // returns leaves_qty after matching
    void restore(Order order); // insert directly without matching (crash recovery)
    bool cancel(const std::string& order_id);
    int  replace(const std::string& order_id, double new_price, int new_qty); // returns new leaves_qty, -1 if not found
    int  available_to_fill(const Order& order) const;

    std::vector<BookLevel> getBids() const;
    std::vector<BookLevel> getAsks() const;
    std::vector<Order> getOrders() const;

private:
    using BidMap = absl::btree_map<double, LevelId, std::greater<double>>;
    using AskMap = absl::btree_map<double, LevelId>;

    struct PriceLevel {
        double price{0.0};
        char side{'0'};
        int total_qty{0};
        OrderId head{kInvalidOrder};
        OrderId tail{kInvalidOrder};
        bool active{false};
    };

    struct OrderStore {
        void reserve(std::size_t n) {
            clord_id.reserve(n);
            exchange_id.reserve(n);
            client_id.reserve(n);
            type.reserve(n);
            qty.reserve(n);
            leaves_qty.reserve(n);
            tif.reserve(n);
            arrival_ns.reserve(n);
            dequeue_ns.reserve(n);
            next.reserve(n);
            prev.reserve(n);
            level.reserve(n);
            live.reserve(n);
        }

        std::size_t size() const { return live.size(); }

        std::vector<std::string> clord_id;
        std::vector<std::string> exchange_id;
        std::vector<std::string> client_id;
        std::vector<char> type;
        std::vector<int> qty;
        std::vector<int> leaves_qty;
        std::vector<char> tif;
        std::vector<int64_t> arrival_ns;
        std::vector<int64_t> dequeue_ns;
        std::vector<OrderId> next;
        std::vector<OrderId> prev;
        std::vector<LevelId> level;
        std::vector<std::uint8_t> live;
    };

    void try_match(Order& aggressor);
    template<typename BookSide>
    void match_against(Order& aggressor, BookSide& opposite, bool is_buy);
    Fill make_fill(const Order& order, double price, int qty, int leaves) const;
    Fill make_fill(OrderId id, double price, int qty, int leaves) const;
    Order materialize_order(OrderId id) const;
    double order_price(OrderId id) const;
    char order_side(OrderId id) const;
    void rest_order(Order order);
    LevelId get_or_create_level(char side, double price);
    PriceLevel& level(LevelId id);
    const PriceLevel& level(LevelId id) const;
    OrderId allocate_order(Order order, LevelId level_id);
    void free_order(OrderId id);
    void append_to_level(LevelId level_id, OrderId order_id);
    void unlink_from_level(OrderId order_id);
    void erase_level_if_empty(LevelId level_id);
    bool remove_resting_order(OrderId id);
#ifndef NDEBUG
    void assert_invariants() const;
#endif

    std::string symbol_;
    FillCallback on_fill_;
    BidMap bids_;
    AskMap asks_;
    std::vector<PriceLevel> levels_;
    OrderStore orders_;
    std::vector<OrderId> free_orders_;
    std::vector<LevelId> free_levels_;
    absl::flat_hash_map<std::string, OrderId> order_index_;
    long long exec_seq_{0};
};

} // namespace engine
