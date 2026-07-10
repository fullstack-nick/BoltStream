#pragma once

#include "boltstream/broker/options.h"
#include "boltstream/build_info.h"
#include "boltstream/observability/metrics.h"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace boltstream::broker {

class BrokerRuntime;

class BrokerServer {
public:
  BrokerServer(ServerOptions options, BuildInfo build_info);
  BrokerServer(const BrokerServer&) = delete;
  BrokerServer& operator=(const BrokerServer&) = delete;
  ~BrokerServer();

  void start();
  void stop();
  void wait_for_shutdown_signal();

  [[nodiscard]] bool ready() const { return ready_; }
  [[nodiscard]] std::string version_json() const;
  [[nodiscard]] std::string metrics_text() const;
  [[nodiscard]] std::uint16_t broker_port() const;
  [[nodiscard]] std::uint16_t admin_port() const;

private:
  using Tcp = boost::asio::ip::tcp;

  void prepare_data_directory();
  void schedule_retention();
  void accept_broker_client();
  void accept_admin_client();
  void handle_broker_client(Tcp::socket socket);
  void handle_admin_client(Tcp::socket socket);
  void set_ready_detail(std::string detail);
  [[nodiscard]] std::string ready_detail() const;

  [[nodiscard]] Tcp::endpoint make_endpoint(const Endpoint& endpoint) const;
  [[nodiscard]] std::string health_json(std::string_view status) const;
  [[nodiscard]] std::string http_response(std::string_view status, std::string_view content_type,
                                          std::string_view body,
                                          std::string_view extra_headers = {}) const;

  ServerOptions options_;
  BuildInfo build_info_;
  std::string startup_time_utc_;
  std::chrono::steady_clock::time_point startup_monotonic_;
  std::atomic_bool ready_{false};
  std::atomic_bool stopping_{false};
  std::atomic<std::uint32_t> active_broker_sessions_{0};
  mutable std::mutex ready_detail_mutex_;
  std::string ready_detail_{"starting"};

  boost::asio::io_context io_;
  Tcp::acceptor broker_acceptor_;
  Tcp::acceptor admin_acceptor_;
  boost::asio::steady_timer retention_timer_;
  observability::MetricsRegistry metrics_;
  std::unique_ptr<BrokerRuntime> runtime_;
};

std::string utc_now_iso8601();

} // namespace boltstream::broker
