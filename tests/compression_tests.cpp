#include "boltstream/compression/compression.h"
#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {

std::filesystem::path temp_dir(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         (std::string{name} + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(CompressionTests, NoneAndZstdRoundTripWithinBounds) {
  const std::vector<std::uint8_t> input(4096, static_cast<std::uint8_t>('A'));
  for (const auto codec :
       {boltstream::compression::Codec::None, boltstream::compression::Codec::Zstd}) {
    const auto encoded = boltstream::compression::compress(codec, input, 3);
    EXPECT_EQ(boltstream::compression::decompress(codec, encoded, input.size(), input.size()),
              input);
  }
  const auto encoded =
      boltstream::compression::compress(boltstream::compression::Codec::Zstd, input, 3);
  EXPECT_THROW((void)boltstream::compression::decompress(boltstream::compression::Codec::Zstd,
                                                         encoded, input.size(), input.size() - 1),
               std::invalid_argument);
}

TEST(CompressionTests, BatchProtocolRoundTripsAndRejectsCountMismatch) {
  const std::vector<boltstream::protocol::BatchRecord> records{{{'k', '1'}, {'v', '1'}},
                                                               {{'k', '2'}, {'v', '2'}}};
  const auto raw = boltstream::protocol::encode_record_set(records);
  const auto encoded =
      boltstream::compression::compress(boltstream::compression::Codec::Zstd, raw, 3);
  boltstream::protocol::ProduceBatchRequest request{
      "topic", 1, boltstream::compression::Codec::Zstd, 2, static_cast<std::uint32_t>(raw.size()),
      encoded};
  boltstream::protocol::ProduceBatchRequest decoded;
  EXPECT_TRUE(boltstream::protocol::decode_produce_batch_request(
                  boltstream::protocol::encode_produce_batch_request(request), decoded)
                  .ok);
  std::vector<boltstream::protocol::BatchRecord> decoded_records;
  EXPECT_TRUE(boltstream::protocol::decode_record_set(raw, 2, decoded_records).ok);
  EXPECT_EQ(decoded_records.size(), 2U);
  EXPECT_FALSE(boltstream::protocol::decode_record_set(raw, 3, decoded_records).ok);
}

TEST(CompressionTests, StorageReadsMixedLegacyAndCompressedBatchesAfterRecovery) {
  const auto root = temp_dir("boltstream-compression-test");
  {
    auto log = boltstream::storage::PartitionLog::open({root, "events", 0});
    const std::vector<std::uint8_t> legacy_key{'l'};
    const std::vector<std::uint8_t> legacy_value{'o', 'l', 'd'};
    log.append(legacy_key, legacy_value);
    const std::vector<std::uint8_t> repeated(256, static_cast<std::uint8_t>('A'));
    const std::vector<boltstream::protocol::BatchRecord> records{{{'a'}, repeated},
                                                                 {{'b'}, repeated}};
    const auto raw = boltstream::protocol::encode_record_set(records);
    const auto encoded =
        boltstream::compression::compress(boltstream::compression::Codec::Zstd, raw, 3);
    std::vector<boltstream::storage::AppendRecord> appends;
    for (const auto& record : records) {
      appends.push_back({record.key, record.message});
    }
    const auto metadata = log.append_encoded_batch(boltstream::compression::Codec::Zstd, encoded,
                                                   static_cast<std::uint32_t>(raw.size()), appends);
    EXPECT_EQ(metadata.base_offset, 1U);
    EXPECT_EQ(metadata.next_offset, 3U);
    EXPECT_LT(metadata.encoded_bytes, metadata.logical_bytes);
  }
  {
    auto recovered = boltstream::storage::PartitionLog::open({root, "events", 0});
    const auto records = recovered.read_from(0, 10, 0);
    ASSERT_EQ(records.size(), 3U);
    EXPECT_EQ(records[0].value, (std::vector<std::uint8_t>{'o', 'l', 'd'}));
    EXPECT_EQ(records[2].value.size(), 256U);
  }
  std::filesystem::remove_all(root);
}

TEST(CompressionTests, MetadataNegotiationKeepsVersionFourShape) {
  boltstream::protocol::MetadataResponse response;
  response.supported_codecs = boltstream::compression::kSupportedCodecMask;
  response.negotiated_codecs = boltstream::compression::kSupportedCodecMask;
  response.topics.push_back({"topic", 0, 3});
  boltstream::protocol::MetadataResponse decoded;
  EXPECT_TRUE(boltstream::protocol::decode_metadata_response(
                  boltstream::protocol::encode_metadata_response(response, 5), decoded, 5)
                  .ok);
  EXPECT_EQ(decoded.negotiated_codecs, boltstream::compression::kSupportedCodecMask);
  EXPECT_TRUE(boltstream::protocol::decode_metadata_response(
                  boltstream::protocol::encode_metadata_response(response, 4), decoded, 4)
                  .ok);
}

} // namespace
