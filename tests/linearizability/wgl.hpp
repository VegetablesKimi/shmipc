#pragma once

// Linearizability checking for FIFO queues: the Wing & Gong search with
// Lowe's memoisation ("WGL").
//
// A history is a set of completed operations, each with an invocation and a
// response timestamp taken from one global seq_cst counter, so the
// timestamps respect real-time order. The history is linearizable if the
// operations can be put in a sequential order that
//   (1) respects real time: if A responded before B was invoked, A comes first;
//   (2) is legal for a sequential FIFO queue: each dequeue returns the oldest
//       value, or "empty" exactly when the queue is empty.
//
// Search: repeatedly pick an operation that may go first (no remaining
// operation responded before it was invoked), apply it to a model queue if
// its recorded result matches, and recurse; backtrack on failure. States
// already shown to fail ((set of linearised ops, queue contents)) are
// memoised. Histories here have at most 64 operations (a bitmask).
//
// Spec::kRelaxedEmpty weakens (2) for dequeues that return "empty". They
// are also legal while some enqueue is still in progress in REAL TIME
// (its interval overlaps the dequeue's), wherever that enqueue sits in the
// linearisation order. That is the guarantee an Agrona-style MPSC actually
// gives. An enqueue takes its queue position at the claim (early), but
// becomes visible at its commit (late), and the consumer stops at the oldest
// claimed-but-uncommitted record. So it can report "empty" even though
// later-claimed enqueues have completed, for as long as that early claim is
// unfinished. A first version required the in-progress enqueue to be not
// yet linearised. That is wrong for exactly this case: the slow enqueue must
// be linearised early (it is dequeued first), and the checker then flagged
// correct MPSC histories.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace shmipc::lin {

enum class Kind { kEnqueue, kDequeue };

struct Op {
  Kind kind;
  std::uint64_t value;  // enqueue: the value; dequeue: the value returned (if !empty)
  bool empty;           // dequeue only: returned "empty"
  std::uint64_t invoke;
  std::uint64_t response;
};

enum class Spec { kStrictFifo, kRelaxedEmpty };

class Checker {
 public:
  Checker(std::vector<Op> ops, Spec spec) : ops_(std::move(ops)), spec_(spec) {}

  bool linearizable() {
    if (ops_.size() > 64) return false;  // out of scope for the bitmask
    std::deque<std::uint64_t> q;
    return search(0, q);
  }

 private:
  bool search(std::uint64_t done, std::deque<std::uint64_t>& q) {
    const std::uint64_t all = ops_.size() == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << ops_.size()) - 1;
    if (done == all) return true;
    if (!seen_.insert(key(done, q)).second) return false;  // memo: this state already failed

    // An op may go next only if no remaining op responded before it was invoked.
    std::uint64_t min_response = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 0; i < ops_.size(); ++i) {
      if (!(done >> i & 1)) min_response = std::min(min_response, ops_[i].response);
    }
    for (std::size_t i = 0; i < ops_.size(); ++i) {
      if ((done >> i & 1) || ops_[i].invoke > min_response) continue;
      const Op& op = ops_[i];
      if (op.kind == Kind::kEnqueue) {
        q.push_back(op.value);
        if (search(done | (std::uint64_t{1} << i), q)) return true;
        q.pop_back();
      } else if (op.empty) {
        if (q.empty() || (spec_ == Spec::kRelaxedEmpty && concurrent_enqueue(op))) {
          if (search(done | (std::uint64_t{1} << i), q)) return true;
        }
      } else if (!q.empty() && q.front() == op.value) {
        q.pop_front();
        if (search(done | (std::uint64_t{1} << i), q)) return true;
        q.push_front(op.value);
      }
    }
    return false;
  }

  // Some enqueue overlaps the dequeue in real time.
  bool concurrent_enqueue(const Op& d) const {
    for (const Op& e : ops_) {
      if (e.kind == Kind::kEnqueue && e.invoke < d.response && d.invoke < e.response) return true;
    }
    return false;
  }

  static std::string key(std::uint64_t done, const std::deque<std::uint64_t>& q) {
    std::string k(reinterpret_cast<const char*>(&done), sizeof(done));
    for (std::uint64_t v : q) k.append(reinterpret_cast<const char*>(&v), sizeof(v));
    return k;
  }

  std::vector<Op> ops_;
  Spec spec_;
  std::unordered_set<std::string> seen_;
};

inline bool linearizable(std::vector<Op> ops, Spec spec) { return Checker(std::move(ops), spec).linearizable(); }

}  // namespace shmipc::lin
