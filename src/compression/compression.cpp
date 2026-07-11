#include "boltstream/compression/compression.h"

#include <zstd.h>

#include <stdexcept>

namespace boltstream::compression {

std::string_view codec_name(Codec codec) {
  switch (codec) {
  case Codec::None:
    return "none";
  case Codec::Zstd:
    return "zstd";
  }
  return "unknown";
}

bool is_supported(Codec codec) { return codec == Codec::None || codec == Codec::Zstd; }

std::vector<std::uint8_t> compress(Codec codec, std::span<const std::uint8_t> input,
                                   int zstd_level) {
  if (codec == Codec::None) {
    return {input.begin(), input.end()};
  }
  if (codec != Codec::Zstd) {
    throw std::invalid_argument("unsupported compression codec");
  }
  std::vector<std::uint8_t> output(ZSTD_compressBound(input.size()));
  const auto size =
      ZSTD_compress(output.data(), output.size(), input.data(), input.size(), zstd_level);
  if (ZSTD_isError(size) != 0U) {
    throw std::runtime_error(std::string{"zstd compression failed: "} + ZSTD_getErrorName(size));
  }
  output.resize(size);
  return output;
}

std::vector<std::uint8_t> decompress(Codec codec, std::span<const std::uint8_t> input,
                                     std::size_t expected_size, std::size_t maximum_size) {
  if (expected_size > maximum_size) {
    throw std::invalid_argument("uncompressed batch exceeds configured limit");
  }
  if (codec == Codec::None) {
    if (input.size() != expected_size) {
      throw std::invalid_argument("uncompressed batch size mismatch");
    }
    return {input.begin(), input.end()};
  }
  if (codec != Codec::Zstd) {
    throw std::invalid_argument("unsupported compression codec");
  }
  std::vector<std::uint8_t> output(expected_size);
  const auto size = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
  if (ZSTD_isError(size) != 0U) {
    throw std::runtime_error(std::string{"zstd decompression failed: "} + ZSTD_getErrorName(size));
  }
  if (size != expected_size) {
    throw std::invalid_argument("decompressed batch size mismatch");
  }
  return output;
}

} // namespace boltstream::compression
