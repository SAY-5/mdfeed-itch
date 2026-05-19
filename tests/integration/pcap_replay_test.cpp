#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "core/depth_book.h"
#include "core/feed_handler.h"
#include "core/parser.h"
#include "pcap/pcap_io.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

// The committed pcaps live at tests/pcap/. Tests run from the build dir, so
// the path is resolved relative to the source tree via the macro the build
// injects, falling back to a relative path for direct invocation.
#ifndef PCAP_DIR
#define PCAP_DIR "tests/pcap"
#endif

std::string pcap_path(const char* name) {
    return std::string(PCAP_DIR) + "/" + name;
}

// Build the reference book straight from the capture: read every packet, strip
// the transport header, decode the ITCH frame, and apply it directly to a
// DepthBook. This bypasses the FeedHandler's gap-detection and recovery path
// entirely, so it is an independent reconstruction of the same committed
// bytes. The simulator is not re-run here on purpose: std::uniform_int_-
// distribution is not portable across standard libraries, so regenerating the
// stream would diverge from the committed pcap on a different platform. The
// pcap is the authoritative simulator output; decoding it is the reference.
itch::DepthBook reference_book_from_pcap(const std::string& path) {
    pcap::PcapReader reader;
    std::string err;
    EXPECT_TRUE(reader.open(path, &err)) << err;
    itch::DepthBook book(10);
    pcap::PcapRecord rec;
    while (reader.next(rec)) {
        if (rec.payload.size() <= sim::kTransportHeaderSize) continue;
        const std::uint8_t* body = rec.payload.data() + sim::kTransportHeaderSize;
        const std::size_t body_len = rec.payload.size() - sim::kTransportHeaderSize;
        auto pr = itch::decode_frame(std::span<const std::uint8_t>(body, body_len));
        if (pr.status == itch::ParseStatus::Ok && pr.message) book.apply(*pr.message);
    }
    return book;
}

void expect_books_equal(const itch::SymbolBook& a, const itch::SymbolBook& b,
                        const std::string& label) {
    ASSERT_EQ(a.bids().levels.size(), b.bids().levels.size()) << label << " bid level count";
    ASSERT_EQ(a.asks().levels.size(), b.asks().levels.size()) << label << " ask level count";
    for (std::size_t i = 0; i < a.bids().levels.size(); ++i) {
        EXPECT_EQ(a.bids().levels[i].price, b.bids().levels[i].price)
            << label << " bid price " << i;
        EXPECT_EQ(a.bids().levels[i].total_qty, b.bids().levels[i].total_qty)
            << label << " bid qty " << i;
        EXPECT_EQ(a.bids().levels[i].order_count, b.bids().levels[i].order_count)
            << label << " bid count " << i;
    }
    for (std::size_t i = 0; i < a.asks().levels.size(); ++i) {
        EXPECT_EQ(a.asks().levels[i].price, b.asks().levels[i].price)
            << label << " ask price " << i;
        EXPECT_EQ(a.asks().levels[i].total_qty, b.asks().levels[i].total_qty)
            << label << " ask qty " << i;
        EXPECT_EQ(a.asks().levels[i].order_count, b.asks().levels[i].order_count)
            << label << " ask count " << i;
    }
}

}  // namespace

// The simulator uses a single symbol; recover it from the capture by decoding
// the first Add-order message.
itch::Symbol symbol_from_pcap(const std::string& path) {
    pcap::PcapReader reader;
    std::string err;
    EXPECT_TRUE(reader.open(path, &err)) << err;
    pcap::PcapRecord rec;
    while (reader.next(rec)) {
        if (rec.payload.size() <= sim::kTransportHeaderSize) continue;
        const std::uint8_t* body = rec.payload.data() + sim::kTransportHeaderSize;
        const std::size_t body_len = rec.payload.size() - sim::kTransportHeaderSize;
        auto pr = itch::decode_frame(std::span<const std::uint8_t>(body, body_len));
        if (pr.status != itch::ParseStatus::Ok || !pr.message) continue;
        if (const auto* a = std::get_if<itch::AddOrderMsg>(&*pr.message)) return a->symbol;
    }
    return itch::Symbol{};
}

// Replaying the synthetic pcap through the FeedHandler must reproduce the
// simulator's reference book exactly: same levels, prices, quantities, counts.
// The reference is an independent direct decode of the same capture.
TEST(PcapReplay, SyntheticMatchesSimulatorReference) {
    const std::string path = pcap_path("itch_synthetic.pcap");

    pcap::PcapReader reader;
    std::string err;
    ASSERT_TRUE(reader.open(path, &err)) << err;

    core::FeedHandler handler(10);
    pcap::PcapRecord rec;
    std::uint64_t replayed = 0;
    while (reader.next(rec)) {
        handler.on_datagram(rec.payload.data(), rec.payload.size());
        ++replayed;
    }
    // 1 directory packet + 1000 messages.
    EXPECT_EQ(replayed, 1001u);
    EXPECT_EQ(handler.stats().gaps_detected, 0u);
    EXPECT_EQ(handler.stats().parse_errors, 0u);
    EXPECT_EQ(handler.stats().messages_applied, 1000u);

    const itch::Symbol sym = symbol_from_pcap(path);
    const itch::DepthBook ref = reference_book_from_pcap(path);
    const itch::SymbolBook* hb = handler.book().find(sym);
    const itch::SymbolBook* rb = ref.find(sym);
    ASSERT_NE(hb, nullptr);
    ASSERT_NE(rb, nullptr);
    EXPECT_TRUE(hb->invariants_ok());
    // The book is non-trivial: at least one side carries levels.
    EXPECT_FALSE(hb->bids().levels.empty() && hb->asks().levels.empty());
    expect_books_equal(*hb, *rb, "synthetic");
}

// Replaying the gap pcap must surface exactly one gap whose missing range is
// [500, 510]. The pcap omits the packets carrying transport sequence 500..510
// on the single stock_locate, so the detector reports that range and nothing
// else.
TEST(PcapReplay, GapPcapFlagsExactly500To510) {
    pcap::PcapReader reader;
    std::string err;
    ASSERT_TRUE(reader.open(pcap_path("itch_gap.pcap"), &err)) << err;

    core::FeedHandler handler(10);
    handler.set_skip_gaps(true);  // step past the gap so replay continues

    struct GapRange {
        itch::StockLocate loc;
        itch::SequenceNumber first;
        itch::SequenceNumber last;
    };
    std::vector<GapRange> gaps;
    handler.set_gap_observer(
        [&gaps](itch::StockLocate loc, itch::SequenceNumber first, itch::SequenceNumber last) {
            gaps.push_back({loc, first, last});
        });

    pcap::PcapRecord rec;
    std::uint64_t replayed = 0;
    while (reader.next(rec)) {
        handler.on_datagram(rec.payload.data(), rec.payload.size());
        ++replayed;
    }
    // 1 directory + 1000 messages - 11 dropped (500..510 inclusive) = 990.
    EXPECT_EQ(replayed, 990u);
    EXPECT_EQ(handler.stats().parse_errors, 0u);

    ASSERT_EQ(gaps.size(), 1u) << "expected exactly one gap range";
    EXPECT_EQ(gaps[0].first, 500u);
    EXPECT_EQ(gaps[0].last, 510u);
    EXPECT_EQ(handler.stats().gaps_detected, 1u);
}

// The synthetic and gap captures share every packet outside the dropped
// range, so a synthetic replay must report zero gaps: a sanity check that the
// gap is an artifact of the omission, not the generator.
TEST(PcapReplay, SyntheticHasNoGaps) {
    pcap::PcapReader reader;
    std::string err;
    ASSERT_TRUE(reader.open(pcap_path("itch_synthetic.pcap"), &err)) << err;

    core::FeedHandler handler(10);
    handler.set_skip_gaps(true);
    std::uint64_t gap_count = 0;
    handler.set_gap_observer([&gap_count](itch::StockLocate, itch::SequenceNumber,
                                          itch::SequenceNumber) { ++gap_count; });

    pcap::PcapRecord rec;
    while (reader.next(rec)) {
        handler.on_datagram(rec.payload.data(), rec.payload.size());
    }
    EXPECT_EQ(gap_count, 0u);
    EXPECT_EQ(handler.stats().gaps_detected, 0u);
}
