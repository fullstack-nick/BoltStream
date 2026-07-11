#include "boltstream/benchmark/statistics.h"
#include "boltstream/build_info.h"
#include "boltstream/protocol/protocol.h"
#include "boltstream/storage/partition_log.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string workload{"produce-throughput"};
  std::string host{"127.0.0.1"};
  std::uint16_t port{9000};
  std::uint16_t admin_port{9100};
  std::string token;
  std::string profile{"custom"};
  std::string environment{"local"};
  std::string machine_type{"unspecified"};
  std::string topic_prefix{"phase10-bench"};
  std::string topic;
  std::filesystem::path data_dir;
  std::uint32_t partitions{4};
  std::uint32_t clients{16};
  std::uint32_t duration_seconds{30};
  std::uint32_t warmup_seconds{60};
  std::uint64_t messages{100000};
  std::uint64_t warmup_messages{10000};
  std::uint32_t payload_bytes{256};
  std::uint32_t key_bytes{16};
  std::uint32_t repetitions{1};
  std::uint32_t timeout_ms{30000};
  std::uint32_t preload_batch_records{1024};
  bool skip_preload{false};
  std::uint32_t server_io_workers{0};
  std::uint32_t server_append_workers{0};
  std::uint32_t server_append_batch_records{0};
  std::uint32_t server_queue_depth{0};
  std::string server_log_level{"unspecified"};
  std::filesystem::path json_out;
  std::filesystem::path markdown_out;
};

struct Sample {
  std::uint64_t records{0};
  std::uint64_t bytes{0};
  double seconds{0.0};
  std::uint64_t errors{0};
  std::vector<std::uint64_t> latencies_ns;
  std::uint64_t append_batches{0};
  std::uint64_t append_batch_records{0};
};

struct ThreadResult {
  std::uint64_t records{0};
  std::uint64_t bytes{0};
  std::uint64_t errors{0};
  std::vector<std::uint64_t> latencies_ns;
  std::vector<std::uint64_t> partitions;
  std::string error;
};

void usage() {
  std::cout
      << "Usage:\n"
         "  boltstream-bench --dry-run\n"
         "  boltstream-bench prepare-fetch --data-dir PATH --topic NAME [setup options]\n"
         "  boltstream-bench run --workload produce-throughput|produce-latency|fetch-throughput\n"
         "      [--host HOST] [--port PORT] [--admin-port PORT] [--token TOKEN]\n"
         "      [--profile NAME] [--environment NAME] [--machine-type NAME]\n"
         "      [--topic-prefix NAME] [--topic NAME --skip-preload] [--partitions N] [--clients "
         "N]\n"
         "      [--duration-seconds N] [--warmup-seconds N] [--messages N]\n"
         "      [--warmup-messages N] [--payload-bytes N] [--key-bytes N]\n"
         "      [--repetitions N] [--timeout-ms N] [--json-out PATH]\n"
         "      [--markdown-out PATH] [--server-io-workers N]\n"
         "      [--server-append-workers N] [--server-append-batch-records N]\n"
         "      [--server-queue-depth N] [--server-log-level LEVEL]\n";
  std::cout << "Setup options: [--partitions N] [--messages N] [--payload-bytes N] "
               "[--key-bytes N] [--preload-batch-records N] [--json-out PATH]\n";
}

std::string json_escape(std::string_view value) {
  std::ostringstream out;
  for (const auto ch : value) {
    switch (ch) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20U) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned int>(static_cast<unsigned char>(ch)) << std::dec;
      } else {
        out << ch;
      }
    }
  }
  return out.str();
}

template <typename T> bool parse_unsigned(std::string_view text, T& value) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (const auto ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    if (parsed >
        (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(ch - '0')) / 10U) {
      return false;
    }
    parsed = parsed * 10U + static_cast<unsigned>(ch - '0');
  }
  if (parsed > std::numeric_limits<T>::max()) {
    return false;
  }
  value = static_cast<T>(parsed);
  return true;
}

std::string environment_value(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result{value};
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
#endif
}

std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string cpu_model() {
#if defined(_WIN32)
  const auto model = environment_value("PROCESSOR_IDENTIFIER");
  return model.empty() ? "unknown" : model;
#else
  std::ifstream cpuinfo{"/proc/cpuinfo"};
  std::string line;
  while (std::getline(cpuinfo, line)) {
    constexpr std::string_view prefix{"model name"};
    if (line.rfind(prefix, 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) {
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        return value;
      }
    }
  }
  return "unknown";
#endif
}

std::string os_description() {
#if defined(_WIN32)
  return "Windows";
#else
  // clang-format off
  struct utsname name {};
  // clang-format on
  if (uname(&name) == 0) {
    return std::string{name.sysname} + " " + name.release;
  }
  return "Linux";
#endif
}

std::uint64_t memory_bytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#else
  // clang-format off
  struct sysinfo info {};
  // clang-format on
  return sysinfo(&info) == 0
             ? static_cast<std::uint64_t>(info.totalram) * static_cast<std::uint64_t>(info.mem_unit)
             : 0;
#endif
}

class ProtocolClient {
public:
  ProtocolClient(std::string host, std::uint16_t port, std::string_view token,
                 std::uint32_t timeout_ms)
      : socket_(io_) {
    boost::asio::ip::tcp::resolver resolver{io_};
    const auto endpoints = resolver.resolve(host, std::to_string(port));
    boost::asio::connect(socket_, endpoints);
#if defined(_WIN32)
    const auto timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{static_cast<time_t>(timeout_ms / 1000),
                          static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    if (!token.empty()) {
      const auto frame = request(boltstream::protocol::FrameType::AuthRequest,
                                 boltstream::protocol::encode_auth_request(token));
      boltstream::protocol::AuthResponse response;
      const auto decoded = boltstream::protocol::decode_auth_response(frame.payload, response);
      if (!decoded.ok || (response.status != "authenticated" && response.status != "disabled")) {
        throw std::runtime_error("broker authentication failed");
      }
    }
  }

  boltstream::protocol::Frame request(boltstream::protocol::FrameType type,
                                      const std::vector<std::uint8_t>& payload) {
    const auto correlation_id = next_correlation_id_++;
    const auto encoded = boltstream::protocol::encode_frame(type, correlation_id, payload);
    boost::asio::write(socket_, boost::asio::buffer(encoded));

    std::array<std::uint8_t, boltstream::protocol::kFrameHeaderBytes> header_bytes{};
    boost::asio::read(socket_, boost::asio::buffer(header_bytes));
    const auto header = boltstream::protocol::decode_header(
        header_bytes, boltstream::protocol::kDefaultMaxFrameBytes);
    if (!header.ok) {
      throw std::runtime_error("malformed broker response header: " + header.message);
    }
    boltstream::protocol::Frame frame;
    frame.header = header.header;
    frame.payload.resize(frame.header.payload_bytes);
    if (!frame.payload.empty()) {
      boost::asio::read(socket_, boost::asio::buffer(frame.payload));
    }
    if (frame.header.correlation_id != correlation_id) {
      throw std::runtime_error("broker response correlation id mismatch");
    }
    if (frame.header.frame_type == boltstream::protocol::FrameType::ErrorResponse) {
      boltstream::protocol::ErrorResponse error;
      const auto decoded = boltstream::protocol::decode_error_response(frame.payload, error);
      throw std::runtime_error(
          decoded.ok ? std::string{boltstream::protocol::error_code_name(error.code)} + ": " +
                           error.message
                     : "malformed broker error response");
    }
    return frame;
  }

private:
  boost::asio::io_context io_;
  boost::asio::ip::tcp::socket socket_;
  std::uint64_t next_correlation_id_{1};
};

boltstream::protocol::ProduceResponse produce(ProtocolClient& client, std::string_view topic,
                                              const std::vector<std::uint8_t>& key,
                                              const std::vector<std::uint8_t>& value) {
  const auto frame =
      client.request(boltstream::protocol::FrameType::ProduceRequest,
                     boltstream::protocol::encode_produce_request(topic, key, value));
  if (frame.header.frame_type != boltstream::protocol::FrameType::ProduceResponse) {
    throw std::runtime_error("unexpected produce response type");
  }
  boltstream::protocol::ProduceResponse response;
  const auto decoded = boltstream::protocol::decode_produce_response(frame.payload, response);
  if (!decoded.ok) {
    throw std::runtime_error("malformed produce response: " + decoded.message);
  }
  return response;
}

void create_topic(const Options& options, std::string_view topic) {
  ProtocolClient client{options.host, options.port, options.token, options.timeout_ms};
  const auto frame = client.request(boltstream::protocol::FrameType::CreateTopicRequest,
                                    boltstream::protocol::encode_create_topic_request(
                                        topic, static_cast<std::uint16_t>(options.partitions)));
  if (frame.header.frame_type != boltstream::protocol::FrameType::CreateTopicResponse) {
    throw std::runtime_error("unexpected create-topic response type");
  }
}

void delete_topic(const Options& options, std::string_view topic) {
  ProtocolClient client{options.host, options.port, options.token, options.timeout_ms};
  boltstream::protocol::DeleteTopicRequest request;
  request.topic = std::string{topic};
  const auto frame = client.request(boltstream::protocol::FrameType::DeleteTopicRequest,
                                    boltstream::protocol::encode_delete_topic_request(request));
  if (frame.header.frame_type != boltstream::protocol::FrameType::DeleteTopicResponse) {
    throw std::runtime_error("unexpected delete-topic response type");
  }
}

std::uint16_t key_partition(std::span<const std::uint8_t> key, std::uint16_t partitions) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : key) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return static_cast<std::uint16_t>(hash % partitions);
}

std::vector<std::uint8_t> key_for_partition(std::uint32_t bytes, std::uint16_t target,
                                            std::uint16_t partitions) {
  if (bytes == 0) {
    return {};
  }
  std::vector<std::uint8_t> key(bytes, static_cast<std::uint8_t>('k'));
  for (std::uint64_t candidate = 0; candidate < 1000000; ++candidate) {
    auto value = candidate;
    for (std::size_t index = 0; index < key.size(); ++index) {
      key[index] = static_cast<std::uint8_t>('a' + (value % 26U));
      value /= 26U;
    }
    if (key_partition(key, partitions) == target) {
      return key;
    }
  }
  throw std::runtime_error("failed to construct deterministic partition key");
}

std::vector<std::uint8_t> payload(std::uint32_t bytes) {
  std::vector<std::uint8_t> value(bytes);
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>('A' + (index % 26U));
  }
  return value;
}

std::uint64_t partition_record_count(std::uint64_t total, std::uint32_t partition,
                                     std::uint32_t partitions) {
  return total / partitions + (partition < total % partitions ? 1U : 0U);
}

void prepare_fetch_storage(const Options& options) {
  if (options.data_dir.empty() || options.topic.empty()) {
    throw std::invalid_argument("prepare-fetch requires --data-dir and --topic");
  }
  if (!boltstream::storage::is_valid_topic_name(options.topic)) {
    throw std::invalid_argument("prepare-fetch topic is invalid");
  }
  if (options.preload_batch_records == 0 || options.preload_batch_records > 1024) {
    throw std::invalid_argument("--preload-batch-records must be between 1 and 1024");
  }

  const auto message = payload(options.payload_bytes);
  std::uint64_t appended = 0;
  for (std::uint32_t partition = 0; partition < options.partitions; ++partition) {
    auto log = boltstream::storage::PartitionLog::open(
        {options.data_dir, options.topic, static_cast<std::uint16_t>(partition)});
    if (log.next_offset() != 0) {
      throw std::runtime_error("prepare-fetch requires empty partitions");
    }
    const auto key = key_for_partition(options.key_bytes, static_cast<std::uint16_t>(partition),
                                       static_cast<std::uint16_t>(options.partitions));
    auto remaining = partition_record_count(options.messages, partition, options.partitions);
    while (remaining > 0) {
      const auto count = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, options.preload_batch_records));
      std::vector<boltstream::storage::AppendRecord> records(count, {key, message});
      const auto metadata = log.append_batch(records);
      if (metadata.size() != count) {
        throw std::runtime_error("prepare-fetch appended an incomplete batch");
      }
      appended += metadata.size();
      remaining -= metadata.size();
    }
  }
  if (appended != options.messages) {
    throw std::runtime_error("prepare-fetch record count mismatch");
  }
}

Sample combine_threads(std::vector<ThreadResult>& threads, Clock::time_point started,
                       Clock::time_point finished) {
  Sample sample;
  sample.seconds = std::chrono::duration<double>(finished - started).count();
  for (auto& thread : threads) {
    sample.records += thread.records;
    sample.bytes += thread.bytes;
    sample.errors += thread.errors;
    sample.latencies_ns.insert(sample.latencies_ns.end(),
                               std::make_move_iterator(thread.latencies_ns.begin()),
                               std::make_move_iterator(thread.latencies_ns.end()));
  }
  return sample;
}

Sample run_produce_throughput(const Options& options, std::string_view topic) {
  std::vector<std::unique_ptr<ProtocolClient>> clients;
  clients.reserve(options.clients);
  for (std::uint32_t index = 0; index < options.clients; ++index) {
    clients.push_back(std::make_unique<ProtocolClient>(options.host, options.port, options.token,
                                                       options.timeout_ms));
  }

  std::vector<ThreadResult> results(options.clients);
  std::vector<std::thread> threads;
  std::latch ready{options.clients};
  std::atomic_bool start{false};
  Clock::time_point warmup_end;
  Clock::time_point measure_start;
  Clock::time_point measure_end;
  const auto message = payload(options.payload_bytes);

  for (std::uint32_t index = 0; index < options.clients; ++index) {
    threads.emplace_back([&, index] {
      auto& result = results[index];
      const auto key = key_for_partition(options.key_bytes,
                                         static_cast<std::uint16_t>(index % options.partitions),
                                         static_cast<std::uint16_t>(options.partitions));
      ready.count_down();
      start.wait(false);
      try {
        while (Clock::now() < warmup_end) {
          (void)produce(*clients[index], topic, key, message);
        }
        while (Clock::now() < measure_end) {
          const auto response = produce(*clients[index], topic, key, message);
          ++result.records;
          result.bytes += response.encoded_byte_size;
        }
      } catch (const std::exception& error) {
        ++result.errors;
        result.error = error.what();
      }
    });
  }

  ready.wait();
  warmup_end = Clock::now() + std::chrono::seconds(options.warmup_seconds);
  measure_start = warmup_end;
  measure_end = measure_start + std::chrono::seconds(options.duration_seconds);
  start.store(true);
  start.notify_all();
  for (auto& thread : threads) {
    thread.join();
  }
  return combine_threads(results, measure_start, Clock::now());
}

Sample run_fixed_produce(const Options& options, std::string_view topic, std::uint64_t messages,
                         std::uint64_t warmup_messages, bool collect_latency,
                         std::vector<std::uint64_t>* partition_counts = nullptr) {
  std::vector<std::unique_ptr<ProtocolClient>> clients;
  clients.reserve(options.clients);
  for (std::uint32_t index = 0; index < options.clients; ++index) {
    clients.push_back(std::make_unique<ProtocolClient>(options.host, options.port, options.token,
                                                       options.timeout_ms));
  }

  std::vector<ThreadResult> results(options.clients);
  for (auto& result : results) {
    result.partitions.resize(options.partitions);
  }
  std::vector<std::thread> threads;
  std::latch ready{options.clients};
  std::latch warmed{options.clients};
  std::atomic_bool start_warmup{false};
  std::atomic_bool start_measure{false};
  std::atomic<std::uint64_t> warmup_sequence{0};
  std::atomic<std::uint64_t> sequence{0};
  Clock::time_point measure_start;
  const auto message = payload(options.payload_bytes);

  for (std::uint32_t index = 0; index < options.clients; ++index) {
    threads.emplace_back([&, index] {
      auto& result = results[index];
      bool warmed_signaled = false;
      const auto key = key_for_partition(options.key_bytes,
                                         static_cast<std::uint16_t>(index % options.partitions),
                                         static_cast<std::uint16_t>(options.partitions));
      ready.count_down();
      start_warmup.wait(false);
      try {
        while (warmup_sequence.fetch_add(1) < warmup_messages) {
          (void)produce(*clients[index], topic, key, message);
        }
        warmed.count_down();
        warmed_signaled = true;
        start_measure.wait(false);
        for (;;) {
          const auto current = sequence.fetch_add(1);
          if (current >= messages) {
            break;
          }
          const auto started = Clock::now();
          const auto response = produce(*clients[index], topic, key, message);
          if (collect_latency) {
            result.latencies_ns.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                    .count()));
          }
          ++result.records;
          result.bytes += response.encoded_byte_size;
          ++result.partitions[response.partition];
        }
      } catch (const std::exception& error) {
        ++result.errors;
        result.error = error.what();
        if (!warmed_signaled) {
          warmed.count_down();
        }
      }
    });
  }

  ready.wait();
  start_warmup.store(true);
  start_warmup.notify_all();
  warmed.wait();
  measure_start = Clock::now();
  start_measure.store(true);
  start_measure.notify_all();
  for (auto& thread : threads) {
    thread.join();
  }
  auto sample = combine_threads(results, measure_start, Clock::now());
  if (partition_counts != nullptr) {
    partition_counts->assign(options.partitions, 0);
    for (const auto& result : results) {
      for (std::size_t partition = 0; partition < result.partitions.size(); ++partition) {
        (*partition_counts)[partition] += result.partitions[partition];
      }
    }
  }
  return sample;
}

Sample run_fetch_throughput(const Options& options, std::string_view topic) {
  std::vector<std::uint64_t> partition_counts;
  if (options.skip_preload) {
    partition_counts.resize(options.partitions);
    for (std::uint32_t partition = 0; partition < options.partitions; ++partition) {
      partition_counts[partition] =
          partition_record_count(options.messages, partition, options.partitions);
    }
  } else {
    const auto preload =
        run_fixed_produce(options, topic, options.messages, 0, false, &partition_counts);
    if (preload.errors != 0 || preload.records != options.messages) {
      throw std::runtime_error("fetch preload did not produce the requested records");
    }
  }

  std::vector<std::unique_ptr<ProtocolClient>> clients;
  clients.reserve(options.partitions);
  for (std::uint32_t partition = 0; partition < options.partitions; ++partition) {
    clients.push_back(std::make_unique<ProtocolClient>(options.host, options.port, options.token,
                                                       options.timeout_ms));
  }
  std::vector<ThreadResult> results(options.partitions);
  std::vector<std::thread> threads;
  std::latch ready{options.partitions};
  std::atomic_bool start{false};
  Clock::time_point measure_start;
  const auto expected_message = payload(options.payload_bytes);

  for (std::uint32_t partition = 0; partition < options.partitions; ++partition) {
    threads.emplace_back([&, partition] {
      auto& result = results[partition];
      std::uint64_t next_offset = 0;
      const auto expected_key =
          key_for_partition(options.key_bytes, static_cast<std::uint16_t>(partition),
                            static_cast<std::uint16_t>(options.partitions));
      ready.count_down();
      start.wait(false);
      try {
        while (result.records < partition_counts[partition]) {
          const auto frame =
              clients[partition]->request(boltstream::protocol::FrameType::FetchRequest,
                                          boltstream::protocol::encode_fetch_request(
                                              topic, static_cast<std::uint16_t>(partition),
                                              std::to_string(next_offset), {}, 0));
          boltstream::protocol::FetchResponse response;
          const auto decoded = boltstream::protocol::decode_fetch_response(frame.payload, response);
          if (!decoded.ok) {
            throw std::runtime_error("malformed fetch response: " + decoded.message);
          }
          if (response.records.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
          for (const auto& record : response.records) {
            if (record.offset != next_offset || record.key != expected_key ||
                record.message != expected_message) {
              throw std::runtime_error("fetch verification failed for partition " +
                                       std::to_string(partition));
            }
            ++result.records;
            result.bytes += record.encoded_byte_size;
            next_offset = record.offset + 1;
          }
        }
      } catch (const std::exception& error) {
        ++result.errors;
        result.error = error.what();
      }
    });
  }

  ready.wait();
  measure_start = Clock::now();
  start.store(true);
  start.notify_all();
  for (auto& thread : threads) {
    thread.join();
  }
  return combine_threads(results, measure_start, Clock::now());
}

std::string http_get(const Options& options, std::string_view path) {
  boost::asio::io_context io;
  boost::asio::ip::tcp::resolver resolver{io};
  boost::asio::ip::tcp::socket socket{io};
  const auto endpoints = resolver.resolve(options.host, std::to_string(options.admin_port));
  boost::asio::connect(socket, endpoints);
  const auto request =
      "GET " + std::string{path} + " HTTP/1.1\r\nHost: boltstream\r\nConnection: close\r\n\r\n";
  boost::asio::write(socket, boost::asio::buffer(request));
  boost::asio::streambuf response;
  boost::system::error_code ec;
  while (boost::asio::read(socket, response, boost::asio::transfer_at_least(1), ec) > 0) {
  }
  if (ec != boost::asio::error::eof) {
    throw boost::system::system_error(ec);
  }
  std::istream input{&response};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::uint64_t metric_value(std::string_view metrics, std::string_view name) {
  const auto body = metrics.find("\r\n\r\n");
  std::istringstream lines{
      std::string{metrics.substr(body == std::string_view::npos ? 0 : body + 4)}};
  std::string line;
  const auto prefix = std::string{name} + " ";
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) == 0) {
      std::uint64_t value = 0;
      if (parse_unsigned(std::string_view{line}.substr(prefix.size()), value)) {
        return value;
      }
    }
  }
  throw std::runtime_error("missing integer metric " + std::string{name});
}

double percentile_sorted(const std::vector<std::uint64_t>& sorted, double quantile) {
  return boltstream::benchmarking::nearest_rank_microseconds(sorted, quantile);
}

double median(const std::vector<double>& values) {
  return boltstream::benchmarking::median(values);
}

double coefficient_of_variation(const std::vector<double>& values) {
  return boltstream::benchmarking::coefficient_of_variation(values);
}

std::string render_json(const Options& options, const std::vector<Sample>& samples) {
  const auto build = boltstream::current_build_info();
  std::vector<double> throughput;
  std::vector<std::uint64_t> all_latencies;
  std::uint64_t total_errors = 0;
  for (const auto& sample : samples) {
    throughput.push_back(sample.seconds > 0.0 ? sample.records / sample.seconds : 0.0);
    all_latencies.insert(all_latencies.end(), sample.latencies_ns.begin(),
                         sample.latencies_ns.end());
    total_errors += sample.errors;
  }
  std::sort(all_latencies.begin(), all_latencies.end());
  auto sorted_throughput = throughput;
  std::sort(sorted_throughput.begin(), sorted_throughput.end());

  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "{\n  \"schema_version\":1,\n";
  out << "  \"generated_at_utc\":\"" << utc_now() << "\",\n";
  out << "  \"environment\":{\"kind\":\"" << json_escape(options.environment)
      << "\",\"machine_type\":\"" << json_escape(options.machine_type) << "\",\"cpu_model\":\""
      << json_escape(cpu_model()) << "\",\"logical_cpus\":" << std::thread::hardware_concurrency()
      << ",\"memory_bytes\":" << memory_bytes() << ",\"os\":\"" << json_escape(os_description())
      << "\",\"compiler\":\"" << json_escape(build.compiler) << "\",\"build_type\":\""
      << json_escape(build.build_type) << "\",\"git_sha\":\"" << json_escape(build.git_sha)
      << "\",\"protocol_version\":\"" << json_escape(build.protocol_version)
      << "\",\"storage_format_version\":\"" << json_escape(build.storage_format_version)
      << "\"},\n";
  out << "  \"broker\":{\"profile\":\"" << json_escape(options.profile)
      << "\",\"io_workers\":" << options.server_io_workers
      << ",\"append_workers\":" << options.server_append_workers
      << ",\"append_batch_records\":" << options.server_append_batch_records
      << ",\"append_queue_depth\":" << options.server_queue_depth
      << ",\"durability\":\"flush\",\"metrics_enabled\":true,\"log_level\":\""
      << json_escape(options.server_log_level) << "\"},\n";
  out << "  \"workload\":{\"name\":\"" << json_escape(options.workload)
      << "\",\"partitions\":" << options.partitions << ",\"clients\":" << options.clients
      << ",\"duration_seconds\":" << options.duration_seconds
      << ",\"warmup_seconds\":" << options.warmup_seconds << ",\"messages\":" << options.messages
      << ",\"warmup_messages\":" << options.warmup_messages
      << ",\"payload_bytes\":" << options.payload_bytes << ",\"key_bytes\":" << options.key_bytes
      << ",\"timeout_ms\":" << options.timeout_ms << ",\"preload_method\":\""
      << (options.skip_preload ? "direct-batched-storage-setup" : "authenticated-protocol")
      << "\",\"preload_batch_records\":" << options.preload_batch_records << "},\n";
  out << "  \"repetitions\":[\n";
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto& sample = samples[index];
    auto latencies = sample.latencies_ns;
    std::sort(latencies.begin(), latencies.end());
    out << "    {\"index\":" << index + 1 << ",\"records\":" << sample.records
        << ",\"bytes\":" << sample.bytes << ",\"seconds\":" << sample.seconds
        << ",\"records_per_second\":"
        << (sample.seconds > 0.0 ? sample.records / sample.seconds : 0.0)
        << ",\"mebibytes_per_second\":"
        << (sample.seconds > 0.0 ? sample.bytes / sample.seconds / 1048576.0 : 0.0)
        << ",\"errors\":" << sample.errors
        << ",\"latency_us\":{\"p50\":" << percentile_sorted(latencies, 0.50)
        << ",\"p95\":" << percentile_sorted(latencies, 0.95)
        << ",\"p99\":" << percentile_sorted(latencies, 0.99)
        << ",\"max\":" << (latencies.empty() ? 0.0 : static_cast<double>(latencies.back()) / 1000.0)
        << "},\"append_batches\":" << sample.append_batches
        << ",\"append_batch_records\":" << sample.append_batch_records << "}";
    out << (index + 1 == samples.size() ? "\n" : ",\n");
  }
  out << "  ],\n";
  out << "  \"summary\":{\"records_per_second\":{\"median\":" << median(throughput)
      << ",\"min\":" << (sorted_throughput.empty() ? 0.0 : sorted_throughput.front())
      << ",\"max\":" << (sorted_throughput.empty() ? 0.0 : sorted_throughput.back())
      << ",\"cv_percent\":" << coefficient_of_variation(throughput)
      << "},\"latency_us\":{\"p50\":" << percentile_sorted(all_latencies, 0.50)
      << ",\"p95\":" << percentile_sorted(all_latencies, 0.95)
      << ",\"p99\":" << percentile_sorted(all_latencies, 0.99) << ",\"max\":"
      << (all_latencies.empty() ? 0.0 : static_cast<double>(all_latencies.back()) / 1000.0)
      << "},\"errors\":" << total_errors << "}\n}\n";
  return out.str();
}

std::string render_markdown(const Options& options, const std::vector<Sample>& samples) {
  std::vector<double> throughput;
  std::vector<std::uint64_t> latencies;
  for (const auto& sample : samples) {
    throughput.push_back(sample.seconds > 0.0 ? sample.records / sample.seconds : 0.0);
    latencies.insert(latencies.end(), sample.latencies_ns.begin(), sample.latencies_ns.end());
  }
  std::sort(latencies.begin(), latencies.end());
  std::ostringstream out;
  out << "| Profile | Workload | Throughput (records/s) | p50 (us) | p95 (us) | p99 (us) | max "
         "(us) |\n";
  out << "| --- | --- | ---: | ---: | ---: | ---: | ---: |\n";
  out << "| " << options.profile << " | " << options.workload << " | " << std::fixed
      << std::setprecision(0) << median(throughput) << " | " << std::setprecision(3)
      << percentile_sorted(latencies, 0.50) << " | " << percentile_sorted(latencies, 0.95) << " | "
      << percentile_sorted(latencies, 0.99) << " | "
      << (latencies.empty() ? 0.0 : static_cast<double>(latencies.back()) / 1000.0) << " |\n";
  return out.str();
}

void write_output(const std::filesystem::path& path, std::string_view content) {
  if (path.empty()) {
    return;
  }
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    throw std::runtime_error("failed to open output " + path.string());
  }
  out << content;
}

Options parse_options(int argc, char** argv) {
  Options options;
  if (const auto env_token = environment_value("BOLTSTREAM_BROKER_TOKEN"); !env_token.empty()) {
    options.token = env_token;
  }
  for (int index = 2; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    auto value = [&]() -> std::string_view {
      if (++index >= argc) {
        throw std::invalid_argument("missing value for " + std::string{arg});
      }
      return argv[index];
    };
    auto u32 = [&](std::uint32_t& target) {
      if (!parse_unsigned(value(), target)) {
        throw std::invalid_argument("invalid value for " + std::string{arg});
      }
    };
    auto u64 = [&](std::uint64_t& target) {
      if (!parse_unsigned(value(), target)) {
        throw std::invalid_argument("invalid value for " + std::string{arg});
      }
    };
    if (arg == "--workload") {
      options.workload = value();
    } else if (arg == "--host") {
      options.host = value();
    } else if (arg == "--port") {
      if (!parse_unsigned(value(), options.port) || options.port == 0) {
        throw std::invalid_argument("invalid --port");
      }
    } else if (arg == "--admin-port") {
      if (!parse_unsigned(value(), options.admin_port) || options.admin_port == 0) {
        throw std::invalid_argument("invalid --admin-port");
      }
    } else if (arg == "--token") {
      options.token = value();
    } else if (arg == "--profile") {
      options.profile = value();
    } else if (arg == "--environment") {
      options.environment = value();
    } else if (arg == "--machine-type") {
      options.machine_type = value();
    } else if (arg == "--topic-prefix") {
      options.topic_prefix = value();
    } else if (arg == "--topic") {
      options.topic = value();
    } else if (arg == "--data-dir") {
      options.data_dir = value();
    } else if (arg == "--partitions") {
      u32(options.partitions);
    } else if (arg == "--clients") {
      u32(options.clients);
    } else if (arg == "--duration-seconds") {
      u32(options.duration_seconds);
    } else if (arg == "--warmup-seconds") {
      u32(options.warmup_seconds);
    } else if (arg == "--messages") {
      u64(options.messages);
    } else if (arg == "--warmup-messages") {
      u64(options.warmup_messages);
    } else if (arg == "--payload-bytes") {
      u32(options.payload_bytes);
    } else if (arg == "--key-bytes") {
      u32(options.key_bytes);
    } else if (arg == "--repetitions") {
      u32(options.repetitions);
    } else if (arg == "--timeout-ms") {
      u32(options.timeout_ms);
    } else if (arg == "--preload-batch-records") {
      u32(options.preload_batch_records);
    } else if (arg == "--skip-preload") {
      options.skip_preload = true;
    } else if (arg == "--server-io-workers") {
      u32(options.server_io_workers);
    } else if (arg == "--server-append-workers") {
      u32(options.server_append_workers);
    } else if (arg == "--server-append-batch-records") {
      u32(options.server_append_batch_records);
    } else if (arg == "--server-queue-depth") {
      u32(options.server_queue_depth);
    } else if (arg == "--server-log-level") {
      options.server_log_level = value();
    } else if (arg == "--json-out") {
      options.json_out = value();
    } else if (arg == "--markdown-out") {
      options.markdown_out = value();
    } else {
      throw std::invalid_argument("unknown argument: " + std::string{arg});
    }
  }
  if (options.workload != "produce-throughput" && options.workload != "produce-latency" &&
      options.workload != "fetch-throughput") {
    throw std::invalid_argument("invalid --workload");
  }
  if (options.partitions == 0 || options.partitions > std::numeric_limits<std::uint16_t>::max() ||
      options.clients == 0 || options.repetitions == 0 || options.messages == 0 ||
      options.payload_bytes == 0) {
    throw std::invalid_argument(
        "partitions, clients, repetitions, messages, and payload bytes must be nonzero");
  }
  if (options.preload_batch_records == 0 || options.preload_batch_records > 1024) {
    throw std::invalid_argument("--preload-batch-records must be between 1 and 1024");
  }
  if (options.skip_preload && (options.workload != "fetch-throughput" || options.topic.empty())) {
    throw std::invalid_argument("--skip-preload requires fetch-throughput and --topic");
  }
  return options;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--dry-run") {
    std::cout << "{\"service\":\"boltstream-bench\",\"status\":\"dry_run\","
                 "\"schema_version\":1,\"published_numbers\":false}\n";
    return 0;
  }
  if (argc == 2 && (std::string_view{argv[1]} == "--help" || std::string_view{argv[1]} == "-h")) {
    usage();
    return 0;
  }
  if (argc < 2 || std::string_view{argv[1]} != "run") {
    if (argc >= 2 && std::string_view{argv[1]} == "prepare-fetch") {
      try {
        const auto options = parse_options(argc, argv);
        prepare_fetch_storage(options);
        const auto output =
            "{\"service\":\"boltstream-bench\",\"status\":\"prepared\",\"topic\":\"" +
            json_escape(options.topic) + "\",\"records\":" + std::to_string(options.messages) +
            "}\n";
        write_output(options.json_out, output);
        std::cout << output;
        return 0;
      } catch (const std::invalid_argument& error) {
        std::cerr << "boltstream-bench: " << error.what() << '\n';
        return 2;
      } catch (const std::exception& error) {
        std::cerr << "boltstream-bench: fetch preparation failed: " << error.what() << '\n';
        return 3;
      }
    }
    usage();
    return 2;
  }

  try {
    const auto options = parse_options(argc, argv);
    std::vector<Sample> samples;
    samples.reserve(options.repetitions);
    for (std::uint32_t repetition = 0; repetition < options.repetitions; ++repetition) {
      const auto topic = options.topic.empty()
                             ? options.topic_prefix + "-" +
                                   std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                                   std::to_string(repetition)
                             : options.topic;
      if (options.topic.empty()) {
        create_topic(options, topic);
      }
      try {
        const auto before = http_get(options, "/metrics");
        Sample sample;
        if (options.workload == "produce-throughput") {
          sample = run_produce_throughput(options, topic);
        } else if (options.workload == "produce-latency") {
          sample =
              run_fixed_produce(options, topic, options.messages, options.warmup_messages, true);
        } else {
          sample = run_fetch_throughput(options, topic);
        }
        const auto after = http_get(options, "/metrics");
        const auto before_batches = metric_value(before, "boltstream_append_batches_total");
        const auto after_batches = metric_value(after, "boltstream_append_batches_total");
        const auto before_records = metric_value(before, "boltstream_append_batch_records_sum");
        const auto after_records = metric_value(after, "boltstream_append_batch_records_sum");
        sample.append_batches = after_batches - before_batches;
        sample.append_batch_records = after_records - before_records;
        samples.push_back(std::move(sample));
      } catch (...) {
        try {
          delete_topic(options, topic);
        } catch (...) {
        }
        throw;
      }
      delete_topic(options, topic);
    }

    const auto json = render_json(options, samples);
    const auto markdown = render_markdown(options, samples);
    write_output(options.json_out, json);
    write_output(options.markdown_out, markdown);
    std::cout << json;

    const auto errors = std::accumulate(
        samples.begin(), samples.end(), std::uint64_t{0},
        [](std::uint64_t total, const auto& sample) { return total + sample.errors; });
    return errors == 0 ? 0 : 3;
  } catch (const std::invalid_argument& error) {
    std::cerr << "boltstream-bench: " << error.what() << "\n\n";
    usage();
    return 2;
  } catch (const boost::system::system_error& error) {
    std::cerr << "boltstream-bench: transport failure: " << error.what() << '\n';
    return error.code() == boost::asio::error::timed_out ||
                   error.code() == boost::asio::error::would_block ||
                   error.code() == boost::asio::error::try_again
               ? 4
               : 3;
  } catch (const std::exception& error) {
    std::cerr << "boltstream-bench: benchmark invalid: " << error.what() << '\n';
    return 3;
  }
}
