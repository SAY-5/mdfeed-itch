# pcap replay

`pcap_replay` reads a libpcap-format capture of ITCH multicast packets and
replays the datagrams through the same `FeedHandler` the live multicast path
uses. It is the offline counterpart to the multicast receiver: identical
decode, gap detection, and book maintenance, sourced from a file instead of a
socket.

## Capture format

`pcap_replay` expects an Ethernet (`DLT_EN10MB`) capture where each packet is
an option-free IPv4 + UDP datagram carrying one ITCH transport frame:

```
Ethernet (14) | IPv4 (20) | UDP (8) | ITCH transport datagram
```

The reader strips the fixed 42-byte header prefix and hands the transport
datagram (the same bytes a `MulticastReceiver` would deliver) to the handler.
This is what a libpcap capture of an ITCH multicast group looks like with no
kernel-bypass and no VLAN tags.

`src/pcap/pcap_io.cpp` wraps `pcap_open_offline` / `pcap_next_ex` for reading
and synthesizes the same layout for writing.

## CLI

```
pcap_replay --pcap <path> [options]

  --pcap <path>          input capture (required)
  --speedup <N>          replay-rate multiplier vs. captured timestamps
                         (default 1.0). N=0 replays as fast as possible.
  --start-offset <secs>  skip packets in the first <secs> of capture time
  --stop-after <count>   stop after replaying <count> datagrams
  --skip-gaps            on a sequence gap, step past it and keep replaying
                         (gap accounting, no recovery); each gap is reported
```

`--speedup 2` replays at twice real time; `--speedup 0.5` at half. The pacing
honors the inter-packet gaps recorded in the capture timestamps.

Output is a JSON object with the replay counts, handler stats, and a
`gap_ranges` array listing every detected gap as an inclusive
`[first_missing, last_missing]` sequence range per stock locate.

## Test inputs

Two captures are committed under `tests/pcap/`:

| File                  | Contents                                            |
|-----------------------|-----------------------------------------------------|
| `itch_synthetic.pcap` | 1 directory packet + 1000 ITCH messages, single symbol, transport sequences 1..1000 contiguous |
| `itch_gap.pcap`       | the same stream with the packets carrying transport sequence 500..510 omitted |

Both are output of `pcap_gen` (seed `0xCAFE`, one symbol). The committed files
are the authoritative test inputs. `make pcaps` regenerates them and is
byte-deterministic on a given platform; note that `std::uniform_int_-
distribution` is not portable across standard libraries, so regenerating on a
different libc++/libstdc++ produces a different (still valid) message stream.
The replay test does not depend on this: it validates the handler against an
independent direct decode of the same committed capture, so it passes on any
platform regardless of which one generated the pcap.

`tests/integration/pcap_replay_test.cpp` asserts:

- Replaying `itch_synthetic.pcap` through the `FeedHandler` reproduces the
  book that a direct decode of the same capture produces, level for level
  (price, quantity, order count). The handler's gap-detection and parse path
  is exercised; the reference bypasses it.
- Replaying `itch_gap.pcap` surfaces exactly one gap with missing range
  `[500, 510]` and nothing else. Sequence numbers are generator arithmetic,
  not RNG, so this holds on every platform.

## Quickstart

```
make build
make pcaps                                              # (re)generate inputs
./build/pcap_replay --pcap tests/pcap/itch_synthetic.pcap --speedup 0
./build/pcap_replay --pcap tests/pcap/itch_gap.pcap --speedup 0 --skip-gaps
```
