// tests/test_async_client.cpp
//
// AsyncRestClient test suite mirroring sync tests + basic pool coverage.
//
// This version is designed to NEVER hang forever:
// - runs io_context on a dedicated thread
// - waits using std::future::wait_for (not Asio timers)
// - if a timeout happens, fails + aborts to avoid wedging CI.
//
// Assumptions about your API:
// - rest_cpp::AsyncRestClient(executor, AsyncRestClientConfiguration)
// - awaitable<Result<Response>> AsyncRestClient::send(Request)
// - awaitable<Result<Response>> get/post convenience
// - Error::Code includes InvalidUrl, Unknown
//
// Adjust include paths if needed.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <httplib.h>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rest_cpp/async_client.hpp"
#include "rest_cpp/config.hpp"
#include "rest_cpp/request.hpp"
#include "rest_cpp/response.hpp"
#include "rest_cpp/result.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

    using tcp = net::ip::tcp;

    // ---------------------
    // Hard watchdog (non-Asio)
    // ---------------------
    struct HardWatchdog {
        explicit HardWatchdog(std::chrono::milliseconds timeout)
            : timeout_(timeout), start_(std::chrono::steady_clock::now()) {
            thread_ = std::thread([this] {
                for (;;) {
                    if (done_.load(std::memory_order_relaxed)) return;
                    auto now = std::chrono::steady_clock::now();
                    if (now - start_ >= timeout_) {
                        std::fprintf(
                            stderr,
                            "\n[ WATCHDOG ] test exceeded %lld ms; aborting "
                            "(deadlock or blocking async code)\n",
                            (long long)timeout_.count());
                        std::fflush(stderr);
                        std::abort();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
        }

        ~HardWatchdog() {
            done_.store(true, std::memory_order_relaxed);
            if (thread_.joinable()) thread_.join();
        }

       private:
        std::chrono::milliseconds timeout_;
        std::chrono::steady_clock::time_point start_;
        std::atomic<bool> done_{false};
        std::thread thread_;
    };

    // ---------------------
    // Test HTTP Server
    // ---------------------
    struct HttpTestServer {
        using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

        explicit HttpTestServer(Handler h, bool honor_keep_alive = false)
            : handler_(std::move(h)),
              honor_keep_alive_(honor_keep_alive) {

            svr_.set_keep_alive_max_count(honor_keep_alive ? 10 : 1); // Limit keep-alive
            svr_.set_keep_alive_timeout(5); // 5 seconds

            auto func = [this](const httplib::Request& req, httplib::Response& res) {
                request_count++;
                {
                    std::lock_guard lk(last_req_mu_);
                    last_method = req.method;
                    last_target = req.target;
                    last_body = req.body;
                }

                // Track inflight
                int cur = inflight.fetch_add(1) + 1;
                int prev = max_inflight.load();
                while(cur > prev && !max_inflight.compare_exchange_weak(prev, cur));

                handler_(req, res);

                if (!honor_keep_alive_) {
                    res.set_header("Connection", "close");
                }

                inflight.fetch_sub(1);
            };

            svr_.Get(".*", func);
            svr_.Post(".*", func);
            svr_.Put(".*", func);
            svr_.Patch(".*", func);
            svr_.Delete(".*", func);
            svr_.Options(".*", func);

            // Bind to ephemeral port
            port_ = svr_.bind_to_any_port("127.0.0.1");

            thread_ = std::thread([this] {
                svr_.listen_after_bind();
            });

            // Wait for server to be ready (poll port)
            // httplib listen_after_bind is blocking, so in thread is correct.
            // bind_to_any_port happens before thread, so port_ is valid.
        }

        ~HttpTestServer() {
            svr_.stop();
            if (thread_.joinable()) thread_.join();
        }

        uint16_t port() const noexcept { return static_cast<uint16_t>(port_); }

        std::atomic<int> request_count{0};
        std::atomic<int> max_inflight{0};
        std::atomic<int> inflight{0};

        std::mutex last_req_mu_;
        std::string last_method;
        std::string last_target;
        std::string last_body;

       private:
        httplib::Server svr_;
        Handler handler_;
        bool honor_keep_alive_;
        std::thread thread_;
        int port_;
    };

    // ---------------------
    // Config helpers
    // ---------------------
    static rest_cpp::AsyncRestClientConfiguration make_async_cfg(
        std::optional<std::string> base_url = std::nullopt) {
        rest_cpp::AsyncRestClientConfiguration cfg{};
        cfg.base_url = std::move(base_url);
        cfg.user_agent = "rest_cpp_gtest_async";
        cfg.max_body_bytes = 1024 * 1024;
        cfg.verify_tls = false;

        cfg.pool_config.max_total_connections = 10;
        cfg.pool_config.max_connections_per_endpoint = 5;
        cfg.pool_config.connection_idle_ttl = std::chrono::milliseconds(30000);
        cfg.pool_config.close_on_shutdown = true;

        cfg.connect_timeout = std::chrono::milliseconds(1000);
        cfg.request_timeout = std::chrono::milliseconds(1000);

        return cfg;
    }

    static std::string make_url(uint16_t port, std::string path) {
        if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

    // ---------------------
    // IO runner + await helper
    // ---------------------
    struct IoThreadRunner {
        IoThreadRunner() : ioc_(1) {}

        void start() {
            guard_.emplace(net::make_work_guard(ioc_));
            thread_ = std::thread([this] { ioc_.run(); });
        }

        void stop() {
            if (guard_) guard_.reset();
            ioc_.stop();
            if (thread_.joinable()) thread_.join();
        }

        ~IoThreadRunner() { stop(); }

        net::io_context& ioc() { return ioc_; }

       private:
        net::io_context ioc_;
        std::optional<net::executor_work_guard<net::io_context::executor_type>>
            guard_;
        std::thread thread_;
    };

    template <class T>
    T await_or_abort(net::io_context& ioc, net::awaitable<T> aw,
                     std::chrono::milliseconds timeout) {
        HardWatchdog wd(timeout + std::chrono::milliseconds(1500));

        auto prom = std::make_shared<std::promise<T>>();
        auto fut = prom->get_future();

        net::co_spawn(
            ioc,
            [aw = std::move(aw), prom]() mutable -> net::awaitable<void> {
                try {
                    T v = co_await std::move(aw);
                    prom->set_value(std::move(v));
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
                co_return;
            },
            net::detached);

        if (fut.wait_for(timeout) != std::future_status::ready) {
            ADD_FAILURE()
                << "Async operation timed out after " << timeout.count()
                << "ms (likely blocking code in async path or pool deadlock).";
            std::abort();
        }

        return fut.get();
    }

}  // namespace

// ---------------------
// Tests
// ---------------------

TEST(AsyncRestClient, GetAbsoluteUrlOk) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.path == "/ok") {
            res.status = 200;
            res.set_content("hello", "text/plain");
            return;
        }
        res.status = 404;
        res.set_content("nope", "text/plain");
    });

    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), make_async_cfg());

    auto r = await_or_abort(runner.ioc(), c.get(make_url(srv.port(), "/ok")),
                            std::chrono::milliseconds(2000));
    ASSERT_FALSE(r.has_error()) << r.error().message;
    EXPECT_EQ(r.value().status_code, 200);
    EXPECT_EQ(r.value().body, "hello");
}

TEST(AsyncRestClient, RelativeUrlWithBaseUrlResolves) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.path == "/api/ping") {
            res.status = 200;
            res.set_content("pong", "text/plain");
            return;
        }
        res.status = 404;
        res.set_content("bad", "text/plain");
    });

    auto base = make_url(srv.port(), "/api");

    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(),
                                make_async_cfg(base));

    auto r = await_or_abort(runner.ioc(), c.get("/ping"),
                            std::chrono::milliseconds(2000));
    ASSERT_FALSE(r.has_error()) << r.error().message;
    EXPECT_EQ(r.value().status_code, 200);
    EXPECT_EQ(r.value().body, "pong");
}

TEST(AsyncRestClient, RelativeUrlWithoutBaseUrlErrors) {
    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(),
                                make_async_cfg(std::nullopt));

    auto r = await_or_abort(runner.ioc(), c.get("/ping"),
                            std::chrono::milliseconds(2000));
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(AsyncRestClient, PostEchoBody) {
    HttpTestServer srv([](auto const& req, auto& res) {
        if (req.method == "POST" && req.path == "/echo") {
            res.status = 200;
            res.set_content(req.body, "text/plain");
            return;
        }
        res.status = 400;
        res.set_content("bad", "text/plain");
    });

    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), make_async_cfg());

    auto r = await_or_abort(runner.ioc(),
                            c.post(make_url(srv.port(), "/echo"), "abc123"),
                            std::chrono::milliseconds(2000));
    ASSERT_FALSE(r.has_error()) << r.error().message;
    EXPECT_EQ(r.value().status_code, 200);
    EXPECT_EQ(r.value().body, "abc123");

    EXPECT_EQ(srv.last_method, "POST");
    EXPECT_EQ(srv.last_target, "/echo");
    EXPECT_EQ(srv.last_body, "abc123");
}

TEST(AsyncRestClient, KeepAliveCloseStillAllowsNextRequest) {
    std::atomic<int> n{0};

    HttpTestServer srv(
        [&](auto const&, auto& res) {
            int k = ++n;
            res.status = 200;
            res.set_content((k == 1) ? "first" : "second", "text/plain");

            // Server wrapper manages connection header based on honor_keep_alive
            // But we can force close if we want to test that specific logic
            // However, this test seems to check if we can make a SECOND request.
            // If we close it, the pool should handle reconnection.
            // Let's rely on the wrapper's logic for now, or explicitly set close if needed.
            if (k == 1) {
                // FORCE close on first request to verify pool reconnects?
                // The orig test did: res.keep_alive(false); res.set(http::field::connection, "close");
                // httplib doesn't strictly have "keep_alive(false)" on RESPONSE easily exposed generally
                // but setting header Connection: close works.
                res.set_header("Connection", "close");
            }
        },
        true);

    IoThreadRunner runner;
    runner.start();

    auto cfg = make_async_cfg();
    cfg.pool_config.max_connections_per_endpoint = 1;
    cfg.pool_config.max_total_connections = 2;

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), cfg);

    auto r1 = await_or_abort(runner.ioc(), c.get(make_url(srv.port(), "/ka")),
                             std::chrono::milliseconds(2000));
    ASSERT_FALSE(r1.has_error()) << r1.error().message;
    EXPECT_EQ(r1.value().body, "first");

    auto r2 = await_or_abort(runner.ioc(), c.get(make_url(srv.port(), "/ka")),
                             std::chrono::milliseconds(2000));
    ASSERT_FALSE(r2.has_error()) << r2.error().message;
    EXPECT_EQ(r2.value().body, "second");

    EXPECT_GE(srv.request_count.load(), 2);
}

TEST(AsyncRestClient, UnknownMethodReturnsError) {
    HttpTestServer srv([](auto const&, auto& res) {
        res.status = 200;
        res.set_content("ok", "text/plain");
    });

    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), make_async_cfg());

    rest_cpp::Request req{};
    req.method = static_cast<rest_cpp::HttpMethod>(0x7f);
    req.url = make_url(srv.port(), "/ok");
    req.headers = {};
    req.body = std::nullopt;

    auto r = await_or_abort(runner.ioc(), c.send(req),
                            std::chrono::milliseconds(2000));
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::Unknown);
}

TEST(AsyncRestClient, InvalidAbsoluteUrlErrors) {
    IoThreadRunner runner;
    runner.start();

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), make_async_cfg());

    auto r = await_or_abort(runner.ioc(), c.get("127.0.0.1:1234/ok"),
                            std::chrono::milliseconds(2000));
    ASSERT_TRUE(r.has_error());
    EXPECT_EQ(r.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(AsyncRestClient, PoolRespectsMaxConnectionsPerEndpoint) {
    HttpTestServer srv(
        [](auto const&, auto& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            res.status = 200;
            res.set_content("ok", "text/plain");
        },
        true);

    IoThreadRunner runner;
    runner.start();

    auto cfg = make_async_cfg();
    cfg.pool_config.max_connections_per_endpoint = 2;
    cfg.pool_config.max_total_connections = 10;

    rest_cpp::AsyncRestClient c(runner.ioc().get_executor(), cfg);

    constexpr int N = 8;
    std::vector<std::future<rest_cpp::Result<rest_cpp::Response>>> futs;
    futs.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto prom = std::make_shared<
            std::promise<rest_cpp::Result<rest_cpp::Response>>>();
        futs.push_back(prom->get_future());

        net::co_spawn(
            runner.ioc(),
            [aw = c.get(make_url(srv.port(), "/slow")),
             prom]() mutable -> net::awaitable<void> {
                try {
                    auto r = co_await std::move(aw);
                    prom->set_value(std::move(r));
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
                co_return;
            },
            net::detached);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (auto& f : futs) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ADD_FAILURE() << "Timed out waiting for concurrent requests.";
            std::abort();
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (f.wait_for(remaining) != std::future_status::ready) {
            ADD_FAILURE() << "Timed out waiting for concurrent request future.";
            std::abort();
        }
        auto r = f.get();
        ASSERT_FALSE(r.has_error()) << r.error().message;
        EXPECT_EQ(r.value().status_code, 200);
    }

    EXPECT_LE(srv.max_inflight.load(), 2);
}
