# Cache-line padding study

## Question

The depth book's hot read path is a linear scan of a per-side level vector
(`SymbolBook::top_bids_` / `top_asks_`, each a `std::vector<PriceLevel>`).
`PriceLevel` is three `std::uint32_t` fields:

```c++
struct PriceLevel {
    Price price;             // u32
    Quantity total_qty;      // u32
    std::uint32_t order_count;  // u32
};
static_assert(sizeof(PriceLevel) == 12);
```

12 bytes is not a power of two and does not divide 64, so consecutive
`PriceLevel` records straddle cache lines: 5.33 records fit per 64-byte line.
The question is whether widening the record to 16 bytes, or aligning the
backing array to a cache line, measurably changes scan throughput.

## Method

`bench/cache_study.cpp` compares three layouts over an identical depth-10
top-of-book scan that sums `total_qty` across every visible level for a large
batch of synthetic books (the shape of a quote-aggregation consumer):

| Layout      | Record size | Notes                                          |
|-------------|-------------|------------------------------------------------|
| `packed12`  | 12 bytes    | the v1 layout                                  |
| `padded16`  | 16 bytes    | one explicit `u32` of padding, naturally aligned |
| `aligned64` | 16 bytes    | backing array starts on a 64-byte boundary     |

Run with `make cache-study` (or `./build/cache_study --books N --iters M`).

## Result

Representative run, Apple M2 Pro, Apple clang 17, `-O3`, 50,000 books,
200 iterations:

| Layout      | `sizeof` | Levels / cache line | Scan rate (levels/sec) |
|-------------|----------|---------------------|------------------------|
| `packed12`  | 12       | 5.33                | ~5.3 - 6.2 billion     |
| `padded16`  | 16       | 4.00                | ~5.3 - 5.4 billion     |
| `aligned64` | 16       | 4.00                | ~5.4 - 5.5 billion     |

The three layouts land within run-to-run noise of each other; `packed12` is
never slower and is sometimes faster.

## Conclusion

For a depth-10 book the packed 12-byte `PriceLevel` is kept. Reasoning:

- A full side is 10 levels = 120 bytes packed, spanning two cache lines. The
  padded layout is 160 bytes, spanning three. Padding makes the working set
  *larger*, so any win from non-straddling records is cancelled by the extra
  line touched.
- The scan is sequential and the hardware prefetcher hides the straddle.
  Cache-line straddling hurts random access, not linear scans.
- There is no false sharing to eliminate: `top_bids_` and `top_asks_` are
  only ever read and written by the single feed-handler thread.

Cache-line alignment / padding is worth revisiting only if the book depth
grows large enough that per-side scans become random-access dominated, or if
a level record is shared across threads. Neither holds in v2. The packed
layout is therefore the correct default and the `static_assert` on
`sizeof(PriceLevel)` documents the intent.

The hot per-order record `OrderState` (symbol 8 + side 1 + price 4 + qty 4 +
ts 8, padded to 32 bytes) already lands at a power-of-two size; two
`OrderState` records fit a cache line exactly, so it needs no change either.
