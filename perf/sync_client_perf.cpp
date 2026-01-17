#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rest_cpp/client.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

static void print_result(const char* label, int iters,
                         std::chrono::nanoseconds total,
                         std::chrono::nanoseconds min,
                         std::chrono::nanoseconds max) {
    const double total_ms =
        std::chrono::duration<double, std::milli>(total).count();
    const double avg_ms = total_ms / iters;
    const double min_ms =
        std::chrono::duration<double, std::milli>(min).count();
    const double max_ms =
        std::chrono::duration<double, std::milli>(max).count();

    std::cout << "\n[ PERF ] " << label << "\n"
              << "        iters=" << iters << " total_ms=" << std::fixed
              << std::setprecision(2) << total_ms << " avg_ms=" << std::fixed
              << std::setprecision(2) << avg_ms << " min_ms=" << std::fixed
              << std::setprecision(2) << min_ms << " max_ms=" << std::fixed
              << std::setprecision(2) << max_ms << "\n";
}

static void print_rps(const char* label, int seconds, std::uint64_t total_reqs,
                      const std::vector<std::uint32_t>& per_sec) {
    std::uint32_t peak = 0;
    for (auto v : per_sec) peak = std::max(peak, v);

    const double avg = seconds > 0 ? (double)total_reqs / (double)seconds : 0.0;

    std::cout << "\n[ PERF ] " << label << "\n"
              << "        duration_s=" << seconds
              << " total_reqs=" << total_reqs << " avg_rps=" << std::fixed
              << std::setprecision(2) << avg << " peak_rps=" << peak << "\n";
}

// Tiny in-process HTTP server (async accept so stop is reliable).
class LocalHttpServer {
   public:
    LocalHttpServer() : ioc_(1), acceptor_(ioc_) {}

    void start() {
        boost::system::error_code ec;

        tcp::endpoint ep{net::ip::make_address("127.0.0.1"), 0};

        acceptor_.open(ep.protocol(), ec);
        if (ec) throw std::runtime_error("acceptor.open: " + ec.message());

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
            throw std::runtime_error("acceptor.set_option: " + ec.message());

        acceptor_.bind(ep, ec);
        if (ec) throw std::runtime_error("acceptor.bind: " + ec.message());

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("acceptor.listen: " + ec.message());

        port_ = acceptor_.local_endpoint().port();

        do_accept();

        // Run the IO loop on background thread.
        thread_ = std::thread([this] { ioc_.run(); });
    }

    void stop() {
        // Idempotent stop
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            return;
        }

        boost::system::error_code ec;
        acceptor_.close(ec);
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

    ~LocalHttpServer() { stop(); }

    uint16_t port() const { return port_; }

   private:
    void do_accept() {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            [this](boost::system::error_code ec, tcp::socket sock) {
                if (!ec) {
                    // Handle the connection on its own detached thread
                    // (keeps server simple; adequate for perf harness).
                    std::thread(&LocalHttpServer::handle_connection, this,
                                std::move(sock))
                        .detach();
                }
                if (!stopped_.load(std::memory_order_relaxed)) {
                    do_accept();
                }
            });
    }

    void handle_connection(tcp::socket sock) {
        beast::tcp_stream stream(std::move(sock));
        beast::flat_buffer buffer;

        for (;;) {
            boost::system::error_code ec;

            http::request<http::string_body> req;
            http::read(stream, buffer, req, ec);
            if (ec == http::error::end_of_stream || ec == net::error::eof)
                break;
            if (ec) break;

            http::response<http::string_body> res;
            res.version(req.version());
            res.set(http::field::server, "rest_cpp-perf-local");
            res.keep_alive(req.keep_alive());

            if (req.method() == http::verb::get && req.target() == "/health") {
                res.result(http::status::ok);
                res.set(http::field::content_type, "text/plain");
                res.body() = "OK";
                res.prepare_payload();
            } else {
                res.result(http::status::not_found);
                res.set(http::field::content_type, "text/plain");
                res.body() = "not found";
                res.prepare_payload();
            }

            http::write(stream, res, ec);
            if (ec) break;
            if (!res.keep_alive()) break;
        }

        boost::system::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
    uint16_t port_{0};
};

class RestClientPerf : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        server_.start();
        cfg_.base_url =
            std::string("http://127.0.0.1:") + std::to_string(server_.port());
        cfg_.user_agent = "rest_cpp-perf";
    }

    static void TearDownTestSuite() { server_.stop(); }

    static inline LocalHttpServer server_{};
    static inline rest_cpp::RestClientConfiguration cfg_{};
};

TEST_F(RestClientPerf, WarmSameClientSameHost) {
    constexpr int iters = 200;
    rest_cpp::RestClient client(cfg_);

    // Warm-up
    {
        auto r = client.get("/health");
        ASSERT_TRUE(r.has_value())
            << (r.has_error() ? r.error().message : "unknown error");
        ASSERT_EQ(r.value().status_code, 200);
    }

    using clock = std::chrono::steady_clock;
    std::chrono::nanoseconds total{0};
    std::chrono::nanoseconds min = std::chrono::nanoseconds::max();
    std::chrono::nanoseconds max{0};

    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        auto r = client.get("/health");
        const auto t1 = clock::now();

        ASSERT_TRUE(r.has_value())
            << (r.has_error() ? r.error().message : "unknown error");
        ASSERT_EQ(r.value().status_code, 200);

        const auto dt =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
        total += dt;
        min = std::min(min, dt);
        max = std::max(max, dt);
    }

    print_result("Warm (same client -> local server)", iters, total, min, max);
}

TEST_F(RestClientPerf, ColdNewClientEachRequest) {
    constexpr int iters = 100;
    using clock = std::chrono::steady_clock;

    std::chrono::nanoseconds total{0};
    std::chrono::nanoseconds min = std::chrono::nanoseconds::max();
    std::chrono::nanoseconds max{0};

    for (int i = 0; i < iters; ++i) {
        rest_cpp::RestClient client(cfg_);

        const auto t0 = clock::now();
        auto r = client.get("/health");
        const auto t1 = clock::now();

        ASSERT_TRUE(r.has_value())
            << (r.has_error() ? r.error().message : "unknown error");
        ASSERT_EQ(r.value().status_code, 200);

        const auto dt =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
        total += dt;
        min = std::min(min, dt);
        max = std::max(max, dt);
    }

    print_result("Cold (new client each req -> local server)", iters, total,
                 min, max);
}

TEST_F(RestClientPerf, MaxRps10Seconds) {
    constexpr int seconds = 10;
    rest_cpp::RestClient client(cfg_);

    // Warm-up
    {
        auto r = client.get("/health");
        ASSERT_TRUE(r.has_value())
            << (r.has_error() ? r.error().message : "unknown error");
    }

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    std::vector<std::uint32_t> per_sec(seconds, 0);
    std::uint64_t total = 0;

    for (;;) {
        const auto now = clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(now - start)
                .count();
        if (elapsed >= seconds) break;

        auto r = client.get("/health");
        ASSERT_TRUE(r.has_value())
            << (r.has_error() ? r.error().message : "unknown error");
        ASSERT_EQ(r.value().status_code, 200);

        ++total;
        ++per_sec[static_cast<std::size_t>(elapsed)];
    }

    print_rps("Max RPS over 30s (same client -> local server)", seconds, total,
              per_sec);
}
