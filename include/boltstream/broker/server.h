#pragma once

#include "boltstream/broker/options.h"
#include "boltstream/build_info.h"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace boltstream::broker {

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

private:
  using Tcp = boost::asio::ip::tcp;

  void prepare_data_directory();
  void accept_broker_clients(std::stop_token stop_token);
  void accept_admin_clients(std::stop_token stop_token);
  void handle_broker_client(Tcp::socket socket);
  void handle_admin_client(Tcp::socket socket);

  [[nodiscard]] Tcp::endpoint make_endpoint(const Endpoint& endpoint) const;
  [[nodiscard]] std::string health_json(std::string_view status) const;
  [[nodiscard]] std::string http_response(std::string_view status, std::string_view content_type,
                                          std::string_view body) const;

  ServerOptions options_;
  BuildInfo build_info_;
  std::string startup_time_utc_;
  std::atomic_bool ready_{false};
  std::string ready_detail_{"starting"};

  boost::asio::io_context io_;
  Tcp::acceptor broker_acceptor_;
  Tcp::acceptor admin_acceptor_;
  std::jthread broker_thread_;
  std::jthread admin_thread_;
};

std::string utc_now_iso8601();

} // namespace boltstream::broker
