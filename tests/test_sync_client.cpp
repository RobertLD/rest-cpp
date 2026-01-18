// test_rest_client.cpp
//
// Minimal fixes:
// 1) Prevent server thread hang by default-closing after each response.
//    (Opt-in keep-alive via constructor flag; only KeepAliveClose test uses
//    it.)
// 2) Handle timeout/end-of-stream cleanly.
// 3) Keep everything else the same.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "rest_cpp/client.hpp"
#include "rest_cpp/config.hpp"
#include "rest_cpp/request.hpp"
#include "rest_cpp/response.hpp"
#include "rest_cpp/result.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

    using tcp = net::ip::tcp;

    struct HttpTestServer {
        using Handler =
            std::function<void(const http::request<http::string_body>&,
                               http::response<http::string_body>&)>;

        explicit HttpTestServer(Handler h, bool honor_keep_alive = false)
            : handler_(std::move(h)),
              honor_keep_alive_(honor_keep_alive),
              ioc_(1),
              acceptor_(ioc_) {
            tcp::endpoint ep{net::ip::make_address("127.0.0.1"), 0};
            beast::error_code ec;

            acceptor_.open(ep.protocol(), ec);
            if (ec) throw beast::system_error(ec);

            acceptor_.set_option(net::socket_base::reuse_address(true), ec);
            if (ec) throw beast::system_error(ec);

            acceptor_.bind(ep, ec);
            if (ec) throw beast::system_error(ec);

            acceptor_.listen(net::socket_base::max_listen_connections, ec);
            if (ec) throw beast::system_error(ec);

            port_ = acceptor_.local_endpoint().port();

            thread_ = std::thread([this] { this->run(); });

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        ~HttpTestServer() {
            stop_.store(true, std::memory_order_relaxed);
            beast::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
            try {
                net::io_context tmp_ioc;
                tcp::socket s(tmp_ioc);
                s.connect(
                    tcp::endpoint(net::ip::make_address("127.0.0.1"), port_));
            } catch (...) {
            }
            if (thread_.joinable()) thread_.join();
        }

        uint16_t port() const noexcept { return port_; }

        std::atomic<int> request_count{0};
        std::string last_method;
        std::string last_target;
        std::string last_body;

       private:
        void run() {
            while (!stop_.load(std::memory_order_relaxed)) {
                beast::error_code ec;
                tcp::socket sock{ioc_};
                acceptor_.accept(sock, ec);
                if (ec) {
                    continue;
                }

                beast::tcp_stream stream(std::move(sock));
                beast::flat_buffer buffer;

                for (;;) {
                    if (stop_.load(std::memory_order_relaxed)) break;

                    // Bound how long we can block waiting for the next request.
                    stream.expires_after(std::chrono::milliseconds(200));
                    http::request<http::string_body> req;
                    http::read(stream, buffer, req, ec);

                    if (ec == beast::error::timeout ||
                        ec == http::error::end_of_stream) {
                        break;
                    }
                    if (ec) break;

                    request_count.fetch_add(1, std::memory_order_relaxed);
                    last_method = std::string(req.method_string());
                    last_target = std::string(req.target());
                    last_body = req.body();

                    http::response<http::string_body> res;
                    res.version(req.version());
                    res.keep_alive(req.keep_alive());

                    handler_(req, res);

                    if (res.result() == http::status::unknown) {
                        res.result(http::status::ok);
                    }
                    if (!res.has_content_length() && !res.body().empty()) {
                        res.prepare_payload();
                    }

                    // Default: close after each response so tests never hang.
                    if (!honor_keep_alive_) {
                        res.keep_alive(false);
                        res.set(http::field::connection, "close");
                    }

                    stream.expires_after(std::chrono::milliseconds(200));
                    http::write(stream, res, ec);
                    if (ec) break;

                    if (!honor_keep_alive_ || !res.keep_alive()) {
                        break;
                    }
                }

                stream.socket().shutdown(tcp::socket::shutdown_both, ec);
                stream.socket().close(ec);
            }
        }

        Handler handler_;
        bool honor_keep_alive_{false};
        net::io_context ioc_;
        tcp::acceptor acceptor_;
        std::thread thread_;
        std::atomic<bool> stop_{false};
        uint16_t port_{0};
    };

    static rest_cpp::RestClientConfiguration make_cfg(
        std::optional<std::string> base_url = std::nullopt) {
        rest_cpp::RestClientConfiguration cfg{};
        cfg.base_url = std::move(base_url);
        cfg.user_agent = "rest_cpp_gtest";
        cfg.max_body_bytes = 1024 * 1024;
        return cfg;
    }

    static std::string make_url(uint16_t port, std::string path) {
        if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

}  // namespace

TEST(RestClientSync, GetAbsoluteUrlOk) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.target() == "/ok") {
            res.result(http::status::ok);
            res.set(http::field::content_type, "text/plain");
            res.body() = "hello";
            res.prepare_payload();
            return;
        }
        res.result(http::status::not_found);
        res.body() = "nope";
        res.prepare_payload();
    });

    rest_cpp::RestClient c(make_cfg());
    auto r = c.get(make_url(srv.port(), "/ok"));
    ASSERT_FALSE(r.has_error()) << r.error().message;

    auto& out = r.value();
    EXPECT_EQ(out.status_code, 200);
    EXPECT_EQ(out.body, "hello");
}

TEST(RestClientSync, RelativeUrlWithBaseUrlResolves) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.target() == "/api/ping") {
            res.result(http::status::ok);
            res.body() = "pong";
            res.prepare_payload();
            return;
        }
        res.result(http::status::not_found);
        res.body() = "bad";
        res.prepare_payload();
    });

    auto base = make_url(srv.port(), "/api");
    rest_cpp::RestClient c(make_cfg(base));

    auto r = c.get("/ping");
    ASSERT_FALSE(r.has_error()) << r.error().message;
    EXPECT_EQ(r.value().status_code, 200);
    EXPECT_EQ(r.value().body, "pong");
}

TEST(RestClientSync, RelativeUrlWithoutBaseUrlErrors) {
    rest_cpp::RestClient c(make_cfg(std::nullopt));

    auto r = c.get("/ping");
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(RestClientSync, PostEchoBody) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.method() == http::verb::post && req.target() == "/echo") {
            res.result(http::status::ok);
            res.set(http::field::content_type, "text/plain");
            res.body() = req.body();
            res.prepare_payload();
            return;
        }
        res.result(http::status::bad_request);
        res.body() = "bad";
        res.prepare_payload();
    });

    rest_cpp::RestClient c(make_cfg());

    auto r = c.post(make_url(srv.port(), "/echo"), "abc123");
    ASSERT_FALSE(r.has_error()) << r.error().message;
    EXPECT_EQ(r.value().status_code, 200);
    EXPECT_EQ(r.value().body, "abc123");

    EXPECT_EQ(srv.last_method, "POST");
    EXPECT_EQ(srv.last_target, "/echo");
    EXPECT_EQ(srv.last_body, "abc123");
}

TEST(RestClientSync, ConvenienceVerbsHitCorrectMethods) {
    HttpTestServer srv([](auto const& req, auto& res) {
        res.result(http::status::ok);
        res.body() =
            std::string(req.method_string()) + " " + std::string(req.target());
        res.prepare_payload();
    });

    rest_cpp::RestClient c(make_cfg());
    {
        auto r = c.get(make_url(srv.port(), "/x"));
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "GET /x");
    }
    {
        auto r = c.head(make_url(srv.port(), "/x"));
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "HEAD /x");
    }
    {
        auto r = c.del(make_url(srv.port(), "/x"));
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "DELETE /x");
    }
    {
        auto r = c.options(make_url(srv.port(), "/x"));
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "OPTIONS /x");
    }
    {
        auto r = c.put(make_url(srv.port(), "/x"), "p");
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "PUT /x");
    }
    {
        auto r = c.patch(make_url(srv.port(), "/x"), "q");
        ASSERT_FALSE(r.has_error());
        EXPECT_EQ(r.value().body, "PATCH /x");
    }
}

TEST(RestClientSync, KeepAliveCloseStillAllowsNextRequest) {
    std::atomic<int> n{0};

    // Honor keep-alive for this test only.
    HttpTestServer srv(
        [&](auto const& req, auto& res) {
            (void)req;
            int k = ++n;
            res.result(http::status::ok);
            res.body() = (k == 1) ? "first" : "second";

            if (k == 1) {
                res.keep_alive(false);
                res.set(http::field::connection, "close");
            } else {
                // Close after the second response too, so the server doesn't
                // wait for a third request.
                res.keep_alive(false);
                res.set(http::field::connection, "close");
            }

            res.prepare_payload();
        },
        /*honor_keep_alive=*/true);

    rest_cpp::RestClient c(make_cfg());

    auto r1 = c.get(make_url(srv.port(), "/ka"));
    ASSERT_FALSE(r1.has_error()) << r1.error().message;
    EXPECT_EQ(r1.value().body, "first");

    auto r2 = c.get(make_url(srv.port(), "/ka"));
    ASSERT_FALSE(r2.has_error()) << r2.error().message;
    EXPECT_EQ(r2.value().body, "second");

    EXPECT_GE(srv.request_count.load(), 2);
}

TEST(RestClientSync, SwitchingEndpointsWorks) {
    HttpTestServer srv1([](auto const& req, auto& res) {
        if (req.target() == "/who") {
            res.result(http::status::ok);
            res.body() = "one";
            res.prepare_payload();
            return;
        }
        res.result(http::status::not_found);
        res.body() = "no";
        res.prepare_payload();
    });
    HttpTestServer srv2([](auto const& req, auto& res) {
        if (req.target() == "/who") {
            res.result(http::status::ok);
            res.body() = "two";
            res.prepare_payload();
            return;
        }
        res.result(http::status::not_found);
        res.body() = "no";
        res.prepare_payload();
    });

    rest_cpp::RestClient c(make_cfg());

    auto r1 = c.get(make_url(srv1.port(), "/who"));
    ASSERT_FALSE(r1.has_error()) << r1.error().message;
    EXPECT_EQ(r1.value().body, "one");

    auto r2 = c.get(make_url(srv2.port(), "/who"));
    ASSERT_FALSE(r2.has_error()) << r2.error().message;
    EXPECT_EQ(r2.value().body, "two");
}

TEST(RestClientSync, UnknownMethodReturnsError) {
    HttpTestServer srv([](auto const& req, auto& res) {
        (void)req;
        res.result(http::status::ok);
        res.body() = "ok";
        res.prepare_payload();
    });

    rest_cpp::RestClient c(make_cfg());

    rest_cpp::Request req{};
    req.method = static_cast<rest_cpp::HttpMethod>(0x7f);
    req.url = make_url(srv.port(), "/ok");
    req.headers = {};
    req.body = std::nullopt;

    auto r = c.send(req);
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::Unknown);
}

TEST(RestClientSync, InvalidAbsoluteUrlErrors) {
    rest_cpp::RestClient c(make_cfg());
    auto r = c.get("127.0.0.1:1234/ok");
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::InvalidUrl);
}
