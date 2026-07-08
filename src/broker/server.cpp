#include "boltstream/broker/server.h"

#include "boltstream/protocol/protocol.h"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
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

class BrokerProtocolSession : public std::enable_shared_from_this<BrokerProtocolSession> {
public:
  using Tcp = boost::asio::ip::tcp;

  BrokerProtocolSession(Tcp::socket socket, std::uint32_t max_frame_bytes,
                        std::function<bool()> ready, std::function<std::string()> ready_detail)
      : socket_(std::move(socket)), max_frame_bytes_(max_frame_bytes), ready_(std::move(ready)),
        ready_detail_(std::move(ready_detail)) {}

  void start() { read_header(); }

private:
  void read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(header_buffer_),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (ec) {
            return;
          }

          const auto decoded = protocol::decode_header(header_buffer_, max_frame_bytes_);
          if (!decoded.ok) {
            write_error(decoded.header.correlation_id, decoded.error, decoded.message, true);
            return;
          }
          read_payload(decoded.header);
        });
  }

  void read_payload(protocol::FrameHeader header) {
    auto self = shared_from_this();
    payload_buffer_.assign(header.payload_bytes, 0);
    boost::asio::async_read(
        socket_, boost::asio::buffer(payload_buffer_),
        [this, self, header](const boost::system::error_code& ec, std::size_t) mutable {
          if (ec) {
            return;
          }

          protocol::Frame frame;
          frame.header = header;
          frame.payload = std::move(payload_buffer_);
          handle_frame(std::move(frame));
        });
  }

  void handle_frame(protocol::Frame frame) {
    std::cerr << "protocol request correlation_id=" << frame.header.correlation_id
              << " type=" << protocol::frame_type_name(frame.header.frame_type)
              << " payload_bytes=" << frame.header.payload_bytes << '\n';

    if (!protocol::is_request_type(frame.header.frame_type)) {
      write_error(frame.header.correlation_id, protocol::ErrorCode::UnsupportedRequest,
                  "frame type is not a client request", false);
      return;
    }

    switch (frame.header.frame_type) {
    case protocol::FrameType::HealthRequest:
      handle_health(std::move(frame));
      return;
    case protocol::FrameType::MetadataRequest:
      handle_empty_not_implemented(std::move(frame), "metadata is implemented in Phase 4");
      return;
    case protocol::FrameType::ProduceRequest:
      handle_validated_not_implemented(std::move(frame), protocol::validate_produce_request,
                                       "produce storage is implemented in Phase 4");
      return;
    case protocol::FrameType::FetchRequest:
      handle_validated_not_implemented(std::move(frame), protocol::validate_fetch_request,
                                       "fetch storage is implemented in Phase 4");
      return;
    case protocol::FrameType::OffsetCommitRequest:
      handle_empty_not_implemented(std::move(frame),
                                   "offset commits are implemented with consumer groups");
      return;
    case protocol::FrameType::AuthRequest:
      handle_empty_not_implemented(std::move(frame), "auth is enforced in a later phase");
      return;
    default:
      write_error(frame.header.correlation_id, protocol::ErrorCode::UnsupportedRequest,
                  "unsupported request frame type", false);
      return;
    }
  }

  void handle_health(protocol::Frame frame) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    const auto payload =
        protocol::encode_health_response(ready_() ? "ready" : "not_ready", ready_detail_());
    write_frame(protocol::FrameType::HealthResponse, frame.header.correlation_id, payload, false);
  }

  using Validator = protocol::DecodeResult (*)(std::span<const std::uint8_t>);

  void handle_validated_not_implemented(protocol::Frame frame, Validator validator,
                                        std::string_view message) {
    const auto validation = validator(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    write_error(frame.header.correlation_id, protocol::ErrorCode::NotImplemented, message, false);
  }

  void handle_empty_not_implemented(protocol::Frame frame, std::string_view message) {
    const auto validation = protocol::validate_empty_payload(frame.payload);
    if (!validation.ok) {
      write_error(frame.header.correlation_id, validation.error, validation.message, true);
      return;
    }
    write_error(frame.header.correlation_id, protocol::ErrorCode::NotImplemented, message, false);
  }

  void write_error(std::uint64_t correlation_id, protocol::ErrorCode code, std::string_view message,
                   bool close_after_write) {
    const auto payload = protocol::encode_error_response(code, message);
    write_frame(protocol::FrameType::ErrorResponse, correlation_id, payload, close_after_write);
  }

  void write_frame(protocol::FrameType frame_type, std::uint64_t correlation_id,
                   std::span<const std::uint8_t> payload, bool close_after_write) {
    auto self = shared_from_this();
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        protocol::encode_frame(frame_type, correlation_id, payload));
    boost::asio::async_write(
        socket_, boost::asio::buffer(*bytes),
        [this, self, bytes, close_after_write](const boost::system::error_code& ec, std::size_t) {
          if (ec || !socket_.is_open()) {
            return;
          }
          if (close_after_write) {
            boost::system::error_code ignored;
            socket_.shutdown(Tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
            return;
          }
          read_header();
        });
  }

  Tcp::socket socket_;
  std::uint32_t max_frame_bytes_;
  std::function<bool()> ready_;
  std::function<std::string()> ready_detail_;
  std::array<std::uint8_t, protocol::kFrameHeaderBytes> header_buffer_{};
  std::vector<std::uint8_t> payload_buffer_;
};

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

  stopping_ = false;
  accept_broker_client();
  accept_admin_client();

  std::cerr << "boltstream-server listening on " << endpoint_to_string(options_.listen)
            << " admin=" << endpoint_to_string(options_.admin_listen)
            << " data=" << options_.data_dir.string() << '\n';
}

void BrokerServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  boost::system::error_code ignored;
  broker_acceptor_.close(ignored);
  admin_acceptor_.close(ignored);
  io_.stop();
}

void BrokerServer::wait_for_shutdown_signal() {
  boost::asio::signal_set signals(io_, SIGINT, SIGTERM);
  signals.async_wait([this](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      std::cerr << "boltstream-server shutting down on signal " << signal_number << '\n';
      stop();
    }
  });
  io_.run();
}

std::string BrokerServer::version_json() const {
  return build_info_json(build_info_, startup_time_utc_);
}

std::uint16_t BrokerServer::broker_port() const {
  boost::system::error_code ec;
  const auto endpoint = broker_acceptor_.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

std::uint16_t BrokerServer::admin_port() const {
  boost::system::error_code ec;
  const auto endpoint = admin_acceptor_.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
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

void BrokerServer::accept_broker_client() {
  auto socket = std::make_shared<Tcp::socket>(io_);
  broker_acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (broker_acceptor_.is_open() && ec != boost::asio::error::operation_aborted) {
        std::cerr << "broker accept error: " << ec.message() << '\n';
      }
      return;
    }
    handle_broker_client(std::move(*socket));
    if (broker_acceptor_.is_open()) {
      accept_broker_client();
    }
  });
}

void BrokerServer::accept_admin_client() {
  auto socket = std::make_shared<Tcp::socket>(io_);
  admin_acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
    if (ec) {
      if (admin_acceptor_.is_open() && ec != boost::asio::error::operation_aborted) {
        std::cerr << "admin accept error: " << ec.message() << '\n';
      }
      return;
    }
    handle_admin_client(std::move(*socket));
    if (admin_acceptor_.is_open()) {
      accept_admin_client();
    }
  });
}

void BrokerServer::handle_broker_client(Tcp::socket socket) {
  std::make_shared<BrokerProtocolSession>(
      std::move(socket), options_.max_frame_bytes, [this] { return ready(); },
      [this] { return ready_detail_; })
      ->start();
}

void BrokerServer::handle_admin_client(Tcp::socket socket) {
  auto client = std::make_shared<Tcp::socket>(std::move(socket));
  auto buffer = std::make_shared<std::array<char, 2048>>();
  client->async_read_some(
      boost::asio::buffer(*buffer),
      [this, client, buffer](const boost::system::error_code& ec, std::size_t read) {
        if (ec && ec != boost::asio::error::eof) {
          return;
        }

        const auto request = std::string_view{buffer->data(), read};
        const auto path = normalize_request_path(request);
        auto response = std::make_shared<std::string>();

        if (path == "/health/live") {
          *response = http_response("200 OK", "application/json", health_json("live"));
        } else if (path == "/health/ready") {
          *response =
              http_response(ready_ ? "200 OK" : "503 Service Unavailable", "application/json",
                            health_json(ready_ ? "ready" : "not_ready"));
        } else if (path == "/version") {
          *response = http_response("200 OK", "application/json", version_json());
        } else {
          *response = http_response("404 Not Found", "application/json",
                                    "{\"status\":\"not_found\",\"service\":\"boltstream\"}");
        }

        boost::asio::async_write(*client, boost::asio::buffer(*response),
                                 [client, response](const boost::system::error_code&, std::size_t) {
                                   boost::system::error_code ignored;
                                   client->shutdown(Tcp::socket::shutdown_both, ignored);
                                 });
      });
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
