#pragma once

// Per-symbol order books rebuilt from ITCH order messages, plus a running
// checksum of every top-of-book change. Two consumers that applied exactly
// the same messages end with the same checksum. That is how the replay demo
// checks end to end that a subscriber saw the feed intact.
//
// Deliberately simple containers (hash map of orders, ordered maps of
// levels). The point of the demo is the transport, not the book. A
// production book would use flat price arrays and an open-addressing order
// table.

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "itch.hpp"

namespace shmipc::itch {

class Books {
 public:
  void on_add(const Header& h, const AddOrder& a) {
    orders_[a.ref] = Order{h.locate, a.side == 'B', a.price, a.shares};
    level(h.locate, a.side == 'B')[a.price] += a.shares;
    touched(h.locate);
  }
  void on_executed(const Header&, const Executed& e) { reduce(e.ref, e.shares); }
  void on_cancel(const Header&, const Cancel& c) { reduce(c.ref, c.shares); }
  void on_delete(const Header&, const Delete& d) {
    auto it = orders_.find(d.ref);
    if (it != orders_.end()) reduce(d.ref, it->second.shares);
  }
  void on_replace(const Header& h, const Replace& r) {
    auto it = orders_.find(r.old_ref);
    if (it == orders_.end()) return;
    const bool bid = it->second.bid;
    reduce(r.old_ref, it->second.shares);
    on_add(h, AddOrder{r.new_ref, bid ? 'B' : 'S', r.shares, r.price});
  }
  void on_other(const Header&) {}

  std::uint64_t checksum() const noexcept { return checksum_; }
  std::size_t live_orders() const noexcept { return orders_.size(); }

  // Best bid / ask for a symbol (0 if that side is empty).
  std::uint32_t best_bid(std::uint16_t locate) const {
    auto it = books_.find(locate);
    return it == books_.end() || it->second.bids.empty() ? 0 : it->second.bids.rbegin()->first;
  }
  std::uint32_t best_ask(std::uint16_t locate) const {
    auto it = books_.find(locate);
    return it == books_.end() || it->second.asks.empty() ? 0 : it->second.asks.begin()->first;
  }

 private:
  struct Order {
    std::uint16_t locate;
    bool bid;
    std::uint32_t price;
    std::uint32_t shares;
  };
  struct Book {
    std::map<std::uint32_t, std::uint64_t> bids;
    std::map<std::uint32_t, std::uint64_t> asks;
  };

  std::map<std::uint32_t, std::uint64_t>& level(std::uint16_t locate, bool bid) {
    Book& b = books_[locate];
    return bid ? b.bids : b.asks;
  }

  void reduce(std::uint64_t ref, std::uint32_t shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    Order& o = it->second;
    const std::uint32_t n = shares < o.shares ? shares : o.shares;
    auto& lv = level(o.locate, o.bid);
    auto l = lv.find(o.price);
    if (l != lv.end()) {
      l->second -= n;
      if (l->second == 0) lv.erase(l);
    }
    const std::uint16_t locate = o.locate;
    o.shares -= n;
    if (o.shares == 0) orders_.erase(it);
    touched(locate);
  }

  void touched(std::uint16_t locate) {
    // Order-sensitive hash of the (symbol, best bid, best ask) sequence.
    std::uint64_t x = checksum_ ^ (std::uint64_t{locate} << 48) ^ (std::uint64_t{best_bid(locate)} << 20) ^
                      best_ask(locate);
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    checksum_ = x ^ (x >> 31);
  }

  std::unordered_map<std::uint64_t, Order> orders_;
  std::unordered_map<std::uint16_t, Book> books_;
  std::uint64_t checksum_ = 0;
};

}  // namespace shmipc::itch
