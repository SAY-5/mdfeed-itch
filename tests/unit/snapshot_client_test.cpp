#include "recovery/snapshot_client.h"

#include <gtest/gtest.h>

using namespace mdfeed::itch;

TEST(SnapshotClient, RequestRoundTrip) {
    SnapshotRequest in{};
    in.stock_locate = 42;
    in.from_seq = 0xCAFEBABEULL;
    const auto bytes = encode_request(in);
    std::size_t consumed = 0;
    const auto dec = decode_frame(bytes, consumed);
    EXPECT_EQ(consumed, bytes.size());
    ASSERT_EQ(dec.kind, DecodedFrame::Kind::Request);
    EXPECT_EQ(dec.request.stock_locate, 42u);
    EXPECT_EQ(dec.request.from_seq, 0xCAFEBABEULL);
}

TEST(SnapshotClient, ResponseRoundTrip) {
    SnapshotResponse in{};
    in.stock_locate = 7;
    in.last_applied_seq = 12345;
    in.symbol = make_symbol("AAPL");
    in.bids.push_back({100, 500, 2});
    in.bids.push_back({99, 300, 1});
    in.asks.push_back({101, 400, 1});
    const auto bytes = encode_response(in);
    std::size_t consumed = 0;
    const auto dec = decode_frame(bytes, consumed);
    EXPECT_EQ(consumed, bytes.size());
    ASSERT_EQ(dec.kind, DecodedFrame::Kind::Response);
    EXPECT_EQ(dec.response.last_applied_seq, 12345u);
    ASSERT_EQ(dec.response.bids.size(), 2u);
    ASSERT_EQ(dec.response.asks.size(), 1u);
    EXPECT_EQ(dec.response.bids[0].price, 100u);
    EXPECT_EQ(dec.response.bids[0].total_qty, 500u);
    EXPECT_EQ(dec.response.bids[0].order_count, 2u);
    EXPECT_EQ(dec.response.asks[0].price, 101u);
    EXPECT_EQ(symbol_to_string(dec.response.symbol), "AAPL");
}

TEST(SnapshotClient, FaultRoundTrip) {
    SnapshotFault in{};
    in.stock_locate = 9;
    in.code = 3;
    const auto bytes = encode_fault(in);
    std::size_t consumed = 0;
    const auto dec = decode_frame(bytes, consumed);
    EXPECT_EQ(consumed, bytes.size());
    ASSERT_EQ(dec.kind, DecodedFrame::Kind::Fault);
    EXPECT_EQ(dec.fault.stock_locate, 9u);
    EXPECT_EQ(dec.fault.code, 3u);
}

TEST(SnapshotClient, ShortBufferIsSafe) {
    std::vector<std::uint8_t> bytes = {0, 0, 0, 50, 'Q'};  // length=50 but body missing
    std::size_t consumed = 0;
    const auto dec = decode_frame(bytes, consumed);
    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(dec.kind, DecodedFrame::Kind::None);
}
