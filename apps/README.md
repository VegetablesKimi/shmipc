# Example applications

Two programs that use the channels the way a trading stack would. Both are built with the release preset into `build/<OS>-release/apps/`.

## `itch`: market data fan-out over Broadcast

An ITCH 5.0 publisher and any number of subscriber processes, each rebuilding per-symbol order books.

```bash
B=build/$(uname -s)-release/apps/itch
$B gen --out=feed.itch --messages=2000000 --symbols=100
$B stats --file=feed.itch
$B replay --file=feed.itch --channel=/itch --rate=1000000 --start-delay-ms=500 &
$B subscribe --channel=/itch &
$B subscribe --channel=/itch --slow-ns=2000 &
wait
```

On an M4 (macOS), 2M messages at 1M msg/s:

```
published 2000000 messages (62.6 MB) in 2.00 s: 1.00 M msg/s; book checksum 33b958c09f7eae7b
received 2000000, lost 0 (laps 0); publisher sent 2000000; books MATCH publisher's
received 834831, lost 1165169 (laps 56); publisher sent 2000000; books not comparable (messages were lost)
```

- **The fast subscriber's books match the publisher's bit for bit.** The publisher appends its own book checksum to an end-of-feed record.
- **The slow subscriber is lapped 56 times, and its count is exact:** received + lost = 2,000,000. It never slowed the publisher or the other subscriber. That is the broadcast contract (DESIGN.md §4b).

**Real NASDAQ data.** NASDAQ publishes full-day TotalView-ITCH 5.0 sample files (several GB each) at `emi.nasdaq.com/ITCH/Nasdaq ITCH/`. Decompress one and use it anywhere `feed.itch` appears above. `itch stats` reports the message mix and the parse + book rate.

The parser handles the message types that change the book (A, F, E, C, X, D, U) plus S, R and P, and skips the rest by frame length. The book uses `std::map` and `std::unordered_map`, which is enough for a demo. A production feed handler would use flat price arrays and an open-addressing order table; that is a project of its own.

## `logger`: binary logging over MPSC

```bash
build/$(uname -s)-release/apps/logger demo --producers=3 --lines=1000000 --out=demo.log
```

- **Hot path** (`binlog::Logger::log`): claim, write the timestamp, producer id, sequence number and raw arguments, commit. The format string never crosses the channel; producers and the daemon share `formats.hpp`, and the record type is the format id.
- **When the ring is full, `log()` drops the line and counts it.** It never blocks the caller.
- **The daemon** drains, renders the `{}` placeholders and writes text. It sleeps with `Backoff`, and calls `unblock()` if a crashed producer leaves a claim behind.

On an M4, three producer processes logging 1M lines each:

```
producer 0: shmipc log() p50=124 p99=333 p99.9=583 p99.99=10463 (ns)
             fprintf     p50=416 p99=624 p99.9=1583 p99.99=17087 (ns)
daemon: 3000003 lines written
```

The `fprintf` row does the same formatting on the calling thread, writing to a buffered `/dev/null`: that is the cost the binary logger moves off the hot path.

The demo's `log()` max is tens of ms because the demo *retries* when the ring is full, so that every line arrives. A full ring therefore shows up as waiting on the daemon. In production use `log()` drops instead.

## Tests

`tests/apps/apps_e2e_test.cpp` (ctest label `stress`) runs both programs' logic across processes: the ITCH fast/slow subscriber checks, and three logger producers into a daemon with every line checked for order and rendering.
