#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace boltstream::compression {

enum class Codec : std::uint8_t { None = 0, Zstd = 1 };

inline constexpr std::uint32_t kNoneMask = 1U << static_cast<unsigned>(Codec::None);
inline constexpr std::uint32_t kZstdMask = 1U << static_cast<unsigned>(Codec::Zstd);
inline constexpr std::uint32_t kSupportedCodecMask = kNoneMask | kZstdMask;

std::string_view codec_name(Codec codec);
bool is_supported(Codec codec);
std::vector<std::uint8_t> compress(Codec codec, std::span<const std::uint8_t> input,
                                   int zstd_level = 3);
std::vector<std::uint8_t> decompress(Codec codec, std::span<const std::uint8_t> input,
                                     std::size_t expected_size, std::size_t maximum_size);

} // namespace boltstream::compression
