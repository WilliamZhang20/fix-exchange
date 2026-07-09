#include "engine/OrderBook.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using engine::BookLevel;
using engine::Fill;
using engine::Order;
using engine::OrderBook;

Order make_order(const std::string& id, char side, double price, int qty,
                 char tif = '0', char type = '2') {
    Order order;
    order.clord_id = id;
    order.exchange_id = id;
    order.client_id = "CLIENT";
    order.symbol = "AAPL";
    order.side = side;
    order.type = type;
    order.price = price;
    order.qty = qty;
    order.leaves_qty = qty;
    order.tif = tif;
    return order;
}

std::vector<std::string> ids(const std::vector<Order>& orders) {
    std::vector<std::string> out;
    for (const auto& order : orders) out.push_back(order.exchange_id);
    return out;
}

void assert_levels_eq(const std::vector<BookLevel>& actual,
                      const std::vector<std::pair<double, int>>& expected) {
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(actual[i].price == expected[i].first);
        assert(actual[i].qty == expected[i].second);
    }
}

void test_fifo_get_orders() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("B1", '1', 100.0, 10));
    book.add(make_order("B2", '1', 100.0, 20));

    assert((ids(book.getOrders()) == std::vector<std::string>{"B1", "B2"}));
    assert_levels_eq(book.getBids(), {{100.0, 30}});
}

void test_partial_and_full_match_updates_head_and_totals() {
    std::vector<std::pair<Fill, Fill>> fills;
    OrderBook book("AAPL", [&](const Fill& maker, const Fill& taker) {
        fills.push_back({maker, taker});
    });

    book.add(make_order("S1", '2', 100.0, 10));
    book.add(make_order("S2", '2', 100.0, 20));

    int leaves = book.add(make_order("B1", '1', 101.0, 6));
    assert(leaves == 0);
    assert_levels_eq(book.getAsks(), {{100.0, 24}});
    assert((ids(book.getOrders()) == std::vector<std::string>{"S1", "S2"}));
    assert(fills.size() == 1);
    assert(fills.back().first.exchange_id == "S1");
    assert(fills.back().first.leaves_qty == 4);

    leaves = book.add(make_order("B2", '1', 101.0, 4));
    assert(leaves == 0);
    assert_levels_eq(book.getAsks(), {{100.0, 20}});
    assert((ids(book.getOrders()) == std::vector<std::string>{"S2"}));
    assert(fills.size() == 2);
    assert(fills.back().first.exchange_id == "S1");
    assert(fills.back().first.leaves_qty == 0);
}

void test_cancel_middle_head_tail_and_only_order() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("B1", '1', 100.0, 10));
    book.add(make_order("B2", '1', 100.0, 20));
    book.add(make_order("B3", '1', 100.0, 30));

    assert(book.cancel("B2"));
    assert((ids(book.getOrders()) == std::vector<std::string>{"B1", "B3"}));
    assert_levels_eq(book.getBids(), {{100.0, 40}});

    assert(book.cancel("B1"));
    assert((ids(book.getOrders()) == std::vector<std::string>{"B3"}));
    assert_levels_eq(book.getBids(), {{100.0, 30}});

    assert(book.cancel("B3"));
    assert(book.getOrders().empty());
    assert(book.getBids().empty());

    book.add(make_order("B4", '1', 99.0, 15));
    assert(book.cancel("B4"));
    assert(book.getBids().empty());
}

void test_replace_reduce_preserves_fifo_and_total() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("B1", '1', 100.0, 10));
    book.add(make_order("B2", '1', 100.0, 20));

    int result = book.replace("B1", 100.0, 4);
    assert(result == 4);
    std::vector<Order> orders = book.getOrders();
    assert((ids(orders) == std::vector<std::string>{"B1", "B2"}));
    assert(orders[0].qty == 4);
    assert(orders[0].leaves_qty == 4);
    assert_levels_eq(book.getBids(), {{100.0, 24}});
}

void test_price_change_replace_loses_priority() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("B1", '1', 100.0, 10));
    book.add(make_order("B2", '1', 100.0, 20));

    int result = book.replace("B1", 99.0, 10);
    assert(result == 10);
    assert((ids(book.getOrders()) == std::vector<std::string>{"B2", "B1"}));
    assert_levels_eq(book.getBids(), {{100.0, 20}, {99.0, 10}});
}

void test_zero_qty_replace_removes_order() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("B1", '1', 100.0, 10));
    book.add(make_order("B2", '1', 100.0, 20));

    assert(book.replace("B1", 100.0, 0) == 0);
    assert((ids(book.getOrders()) == std::vector<std::string>{"B2"}));
    assert_levels_eq(book.getBids(), {{100.0, 20}});
}

void test_fok_availability_after_mutations() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    book.add(make_order("S1", '2', 100.0, 50));
    book.add(make_order("S2", '2', 100.0, 25));
    book.add(make_order("S3", '2', 101.0, 40));

    assert(book.available_to_fill(make_order("FOK", '1', 100.0, 60, '4')) == 75);
    assert(book.available_to_fill(make_order("FOK", '1', 101.0, 100, '4')) == 115);

    assert(book.replace("S2", 100.0, 10) == 10);
    assert(book.cancel("S1"));
    assert(book.add(make_order("B1", '1', 101.0, 5)) == 0);

    assert_levels_eq(book.getAsks(), {{100.0, 5}, {101.0, 40}});
    assert(book.available_to_fill(make_order("FOK", '1', 100.0, 5, '4')) == 5);
    assert(book.available_to_fill(make_order("FOK", '1', 101.0, 45, '4')) == 45);
}

void test_ioc_and_fok_do_not_rest_in_order_book() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    assert(book.add(make_order("IOC", '1', 100.0, 10, '3')) == 10);
    assert(book.add(make_order("FOK", '1', 100.0, 10, '4')) == 10);
    assert(book.getOrders().empty());
    assert(book.getBids().empty());
}

struct RefOrder {
    std::string id;
    double price;
    int qty;
};

std::vector<std::pair<double, int>> ref_levels(
    const std::map<double, std::deque<RefOrder>, std::greater<double>>& ref) {
    std::vector<std::pair<double, int>> out;
    for (const auto& kv : ref) {
        int total = 0;
        for (const auto& order : kv.second) total += order.qty;
        if (total > 0) out.push_back({kv.first, total});
    }
    return out;
}

std::vector<std::string> ref_ids(
    const std::map<double, std::deque<RefOrder>, std::greater<double>>& ref) {
    std::vector<std::string> out;
    for (const auto& kv : ref) {
        for (const auto& order : kv.second) out.push_back(order.id);
    }
    return out;
}

bool remove_ref(std::map<double, std::deque<RefOrder>, std::greater<double>>& ref,
                const std::string& id, RefOrder* removed = nullptr) {
    for (auto it = ref.begin(); it != ref.end(); ++it) {
        std::deque<RefOrder>& q = it->second;
        for (auto order_it = q.begin(); order_it != q.end(); ++order_it) {
            if (order_it->id == id) {
                if (removed) *removed = *order_it;
                q.erase(order_it);
                if (q.empty()) ref.erase(it);
                return true;
            }
        }
    }
    return false;
}

bool find_ref(const std::map<double, std::deque<RefOrder>, std::greater<double>>& ref,
              const std::string& id, RefOrder* found) {
    for (const auto& kv : ref) {
        for (const auto& order : kv.second) {
            if (order.id == id) {
                if (found) *found = order;
                return true;
            }
        }
    }
    return false;
}

bool reduce_ref(std::map<double, std::deque<RefOrder>, std::greater<double>>& ref,
                const std::string& id, int new_qty, RefOrder* old = nullptr) {
    for (auto& kv : ref) {
        for (auto& order : kv.second) {
            if (order.id == id) {
                if (old) *old = order;
                order.qty = new_qty;
                return true;
            }
        }
    }
    return false;
}

void assert_book_matches_ref(
    const OrderBook& book,
    const std::map<double, std::deque<RefOrder>, std::greater<double>>& ref) {
    assert_levels_eq(book.getBids(), ref_levels(ref));
    assert(ids(book.getOrders()) == ref_ids(ref));

    std::set<std::string> seen;
    for (const auto& order : book.getOrders()) {
        assert(order.leaves_qty > 0);
        assert(seen.insert(order.exchange_id).second);
    }
}

void test_randomized_add_cancel_replace_against_reference() {
    OrderBook book("AAPL", [](const Fill&, const Fill&) {});
    std::map<double, std::deque<RefOrder>, std::greater<double>> ref;
    std::vector<std::string> live_ids;
    std::mt19937 rng(7);
    int next_id = 1;

    for (int step = 0; step < 1000; ++step) {
        int op = static_cast<int>(rng() % 100);
        if (live_ids.empty() || op < 45) {
            std::string id = "R" + std::to_string(next_id++);
            double price = 99.0 + static_cast<double>(rng() % 3);
            int qty = 1 + static_cast<int>(rng() % 100);
            book.add(make_order(id, '1', price, qty));
            ref[price].push_back({id, price, qty});
            live_ids.push_back(id);
        } else if (op < 70) {
            std::size_t pos = static_cast<std::size_t>(rng() % live_ids.size());
            std::string id = live_ids[pos];
            assert(book.cancel(id));
            assert(remove_ref(ref, id));
            live_ids.erase(live_ids.begin() + static_cast<std::ptrdiff_t>(pos));
        } else {
            std::size_t pos = static_cast<std::size_t>(rng() % live_ids.size());
            std::string id = live_ids[pos];
            RefOrder old;
            assert(find_ref(ref, id, &old));

            if (old.qty > 1 && (rng() % 2 == 0)) {
                int new_qty = 1 + static_cast<int>(rng() % (old.qty - 1));
                assert(book.replace(id, old.price, new_qty) == new_qty);
                assert(reduce_ref(ref, id, new_qty));
            } else {
                assert(remove_ref(ref, id));
                double new_price = 99.0 + static_cast<double>(rng() % 3);
                if (new_price == old.price) new_price = old.price == 101.0 ? 99.0 : old.price + 1.0;
                assert(book.replace(id, new_price, old.qty) == old.qty);
                ref[new_price].push_back({id, new_price, old.qty});
            }
        }

        assert_book_matches_ref(book, ref);
    }
}

} // namespace

int main() {
    test_fifo_get_orders();
    test_partial_and_full_match_updates_head_and_totals();
    test_cancel_middle_head_tail_and_only_order();
    test_replace_reduce_preserves_fifo_and_total();
    test_price_change_replace_loses_priority();
    test_zero_qty_replace_removes_order();
    test_fok_availability_after_mutations();
    test_ioc_and_fok_do_not_rest_in_order_book();
    test_randomized_add_cancel_replace_against_reference();
    return 0;
}
