#include "boltstream/broker/server.h"

#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace boltstream::broker {
namespace {

std::string normalize_request_path(std::string_view request) {
  const auto line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end);
  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return {};
  }
  const auto second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) {
    return {};
  }
  return std::string{line.substr(first_space + 1, second_space - first_space - 1)};
}

} // namespace

std::string utc_now_iso8601() {
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

BrokerServer::BrokerServer(ServerOptions options, BuildInfo build_info)
    : options_(std::move(options)), build_info_(std::move(build_info)),
      startup_time_utc_(utc_now_iso8601()), broker_acceptor_(io_), admin_acceptor_(io_) {}

BrokerServer::~BrokerServer() { stop(); }

void BrokerServer::start() {
  prepare_data_directory();

  broker_acceptor_.open(Tcp::v4());
  broker_acceptor_.set_option(Tcp::acceptor::reuse_address(true));
  broker_acceptor_.bind(make_endpoint(options_.listen));
  broker_acceptor_.listen();

  admin_acceptor_.open(Tcp::v4());
  admin_acceptor_.set_option(Tcp::acceptor::reuse_address(true));
  admin_acceptor_.bind(make_endpoint(options_.admin_listen));
  admin_acceptor_.listen();

  broker_thread_ =
      std::jthread([this](std::stop_token stop_token) { accept_broker_clients(stop_token); });
  admin_thread_ =
      std::jthread([this](std::stop_token stop_token) { accept_admin_clients(stop_token); });

  std::cerr << "boltstream-server listening on " << endpoint_to_string(options_.listen)
            << " admin=" << endpoint_to_string(options_.admin_listen)
            << " data=" << options_.data_dir.string() << '\n';
}

void BrokerServer::stop() {
  boost::system::error_code ignored;
  broker_acceptor_.close(ignored);
  admin_acceptor_.close(ignored);
  io_.stop();
}

void BrokerServer::wait_for_shutdown_signal() {
  boost::asio::signal_set signals(io_, SIGINT, SIGTERM);
  signals.async_wait([this](const boost::system::error_code&, int) { stop(); });
  io_.run();
}

std::string BrokerServer::version_json() const {
  return build_info_json(build_info_, startup_time_utc_);
}

void BrokerServer::prepare_data_directory() {
  std::error_code ec;
  std::filesystem::create_directories(options_.data_dir, ec);
  if (ec) {
    ready_ = false;
    ready_detail_ = "failed to create data directory: " + ec.message();
    throw std::runtime_error(ready_detail_);
  }

  const auto probe = options_.data_dir / ".boltstream-ready-check";
  {
    std::ofstream out{probe, std::ios::binary | std::ios::trunc};
    if (!out) {
      ready_ = false;
      ready_detail_ = "data directory is not writable";
      throw std::runtime_error(ready_detail_);
    }
    out << "ok\n";
  }
  std::filesystem::remove(probe, ec);

  ready_ = true;
  ready_detail_ = "ready";
}

void BrokerServer::accept_broker_clients(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    boost::system::error_code ec;
    Tcp::socket socket{io_};
    broker_acceptor_.accept(socket, ec);
    if (ec) {
      if (broker_acceptor_.is_open()) {
        std::cerr << "broker accept error: " << ec.message() << '\n';
      }
      return;
    }
    handle_broker_client(std::move(socket));
  }
}

void BrokerServer::accept_admin_clients(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    boost::system::error_code ec;
    Tcp::socket socket{io_};
    admin_acceptor_.accept(socket, ec);
    if (ec) {
      if (admin_acceptor_.is_open()) {
        std::cerr << "admin accept error: " << ec.message() << '\n';
      }
      return;
    }
    handle_admin_client(std::move(socket));
  }
}

void BrokerServer::handle_broker_client(Tcp::socket socket) {
  const std::string body = "BoltStream broker protocol starts in Phase 2.\n";
  boost::system::error_code ignored;
  boost::asio::write(socket, boost::asio::buffer(body), ignored);
  socket.shutdown(Tcp::socket::shutdown_both, ignored);
}

void BrokerServer::handle_admin_client(Tcp::socket socket) {
  std::array<char, 2048> buffer{};
  boost::system::error_code ec;
  const auto read = socket.read_some(boost::asio::buffer(buffer), ec);
  if (ec && ec != boost::asio::error::eof) {
    return;
  }

  const auto request = std::string_view{buffer.data(), read};
  const auto path = normalize_request_path(request);
  std::string response;

  if (path == "/health/live") {
    response = http_response("200 OK", "application/json", health_json("live"));
  } else if (path == "/health/ready") {
    response = http_response(ready_ ? "200 OK" : "503 Service Unavailable", "application/json",
                             health_json(ready_ ? "ready" : "not_ready"));
  } else if (path == "/version") {
    response = http_response("200 OK", "application/json", version_json());
  } else {
    response = http_response("404 Not Found", "application/json",
                             "{\"status\":\"not_found\",\"service\":\"boltstream\"}");
  }

  boost::asio::write(socket, boost::asio::buffer(response), ec);
  socket.shutdown(Tcp::socket::shutdown_both, ec);
}

BrokerServer::Tcp::endpoint BrokerServer::make_endpoint(const Endpoint& endpoint) const {
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(endpoint.host, ec);
  if (ec) {
    throw std::runtime_error("invalid endpoint host " + endpoint.host + ": " + ec.message());
  }
  return {address, endpoint.port};
}

std::string BrokerServer::health_json(std::string_view status) const {
  std::ostringstream out;
  out << "{";
  out << "\"service\":\"boltstream\",";
  out << "\"status\":\"" << status << "\",";
  out << "\"git_sha\":\"" << build_info_.git_sha << "\",";
  out << "\"detail\":\"" << ready_detail_ << "\"";
  out << "}";
  return out.str();
}

std::string BrokerServer::http_response(std::string_view status, std::string_view content_type,
                                        std::string_view body) const {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << "\r\n";
  out << "Content-Type: " << content_type << "\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "\r\n";
  out << body;
  return out.str();
}

} // namespace boltstream::broker
