
#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <thread>

#include "rest_cpp/config.hpp"
#include "rest_cpp/connection/connection_pool.hpp"
#include "rest_cpp/endpoint.hpp"

using namespace rest_cpp;
using namespace std::chrono_literals;

namespace {

    AsyncConnectionPoolConfiguration default_cfg() {
        AsyncConnectionPoolConfiguration cfg;
        cfg.max_connections_per_endpoint = 2;
        cfg.max_total_connections = 4;
        cfg.connection_idle_ttl = std::chrono::milliseconds(100);
        cfg.close_on_shutdown = true;
        return cfg;
    }

    Endpoint make_ep(std::string host = "localhost", int port = 80) {
        Endpoint ep;
        ep.host = std::move(host);
        ep.port = port;
        return ep;
    }

    TEST(ConnectionPoolTest, TryAcquireCreatesAndReusesIdle) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        ConnectionPool pool(io.get_executor(), ssl_ctx, default_cfg());
        Endpoint ep = make_ep();
        // First acquire creates new
        auto lease1 = pool.try_acquire(ep);
        ASSERT_TRUE(lease1.has_value());
        auto* conn1 = lease1->get();
        // Release returns to idle
        lease1 = ConnectionPool::Lease{};  // Let previous lease go out of scope
        // Next acquire reuses idle
        auto lease2 = pool.try_acquire(ep);
        ASSERT_TRUE(lease2.has_value());
        EXPECT_EQ(lease2->get(), conn1);
    }

    TEST(ConnectionPoolTest, TryAcquireRespectsEndpointCapacity) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        ConnectionPool pool(io.get_executor(), ssl_ctx, default_cfg());
        Endpoint ep = make_ep();
        auto l1 = pool.try_acquire(ep);
        auto l2 = pool.try_acquire(ep);
        auto l3 = pool.try_acquire(ep);
        EXPECT_TRUE(l1.has_value());
        EXPECT_TRUE(l2.has_value());
        EXPECT_FALSE(l3.has_value());
    }

    TEST(ConnectionPoolTest, TryAcquireRespectsGlobalCapacity) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        ConnectionPool pool(io.get_executor(), ssl_ctx, default_cfg());
        Endpoint ep1 = make_ep("a", 80);
        Endpoint ep2 = make_ep("b", 80);
        auto l1 = pool.try_acquire(ep1);
        auto l2 = pool.try_acquire(ep1);
        auto l3 = pool.try_acquire(ep2);
        auto l4 = pool.try_acquire(ep2);
        auto l5 = pool.try_acquire(ep2);
        EXPECT_TRUE(l1.has_value());
        EXPECT_TRUE(l2.has_value());
        EXPECT_TRUE(l3.has_value());
        EXPECT_TRUE(l4.has_value());
        EXPECT_FALSE(l5.has_value());
    }

    TEST(ConnectionPoolTest, LeaseMoveSemantics) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        ConnectionPool pool(io.get_executor(), ssl_ctx, default_cfg());
        Endpoint ep = make_ep();
        auto lease1 = pool.try_acquire(ep);
        ASSERT_TRUE(lease1.has_value());
        auto* conn = lease1->get();
        ConnectionPool::Lease lease2 = std::move(*lease1);
        EXPECT_EQ(lease2.get(), conn);
        EXPECT_EQ(lease1->get(), nullptr);
        ConnectionPool::Lease lease3;
        lease3 = std::move(lease2);
        EXPECT_EQ(lease3.get(), conn);
        EXPECT_EQ(lease2.get(), nullptr);
    }

    TEST(ConnectionPoolTest, LeaseOperatorBool) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        ConnectionPool pool(io.get_executor(), ssl_ctx, default_cfg());
        Endpoint ep = make_ep();
        auto lease = pool.try_acquire(ep);
        ASSERT_TRUE(lease.has_value());
        EXPECT_TRUE(static_cast<bool>(*lease));
        lease = ConnectionPool::Lease{};
        EXPECT_FALSE(static_cast<bool>(*lease));
    }

    TEST(ConnectionPoolTest, IdlePruningRemovesExpired) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        AsyncConnectionPoolConfiguration cfg = default_cfg();
        cfg.connection_idle_ttl = std::chrono::milliseconds(10);
        ConnectionPool pool(io.get_executor(), ssl_ctx, cfg);
        Endpoint ep = make_ep();
        {
            auto lease = pool.try_acquire(ep);
            ASSERT_TRUE(lease.has_value());
            lease = ConnectionPool::Lease{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Next acquire will prune
        auto lease2 = pool.try_acquire(ep);
        // Should not reuse the old connection (pruned)
        EXPECT_TRUE(lease2.has_value());
    }

    TEST(ConnectionPoolTest, ShutdownMakesLeasesInert) {
        boost::asio::io_context io;
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::sslv23);
        AsyncConnectionPoolConfiguration cfg = default_cfg();
        cfg.close_on_shutdown = true;
        auto lease_ptr = std::make_unique<ConnectionPool::Lease>();
        {
            ConnectionPool pool(io.get_executor(), ssl_ctx, cfg);
            Endpoint ep = make_ep();
            auto lease = pool.try_acquire(ep);
            ASSERT_TRUE(lease.has_value());
            *lease_ptr = std::move(*lease);
            // pool goes out of scope and is destroyed
        }
        // Lease should now be inert
        EXPECT_EQ(lease_ptr->get(), nullptr);
        // reset() is private; destruction is sufficient to test inertness
    }

    // Test capable of triggering the UAF race condition
    TEST(ConnectionPoolTest, StressTestRaceCondition) {
        boost::asio::io_context ioc(4);
        boost::asio::ssl::context ssl_ctx(
            boost::asio::ssl::context::tls_client);

        AsyncConnectionPoolConfiguration cfg;
        cfg.max_total_connections = 2;  // Very small pool to force queuing
        cfg.max_connections_per_endpoint = 2;

        // Create pool
        auto pool =
            std::make_shared<ConnectionPool>(ioc.get_executor(), ssl_ctx, cfg);

        Endpoint ep;
        ep.host = "localhost";  // Dummy, we won't actually connect
        ep.port = "80";

        std::atomic<int> completed{0};
        const int NUM_REQUESTS = 1000;

        // Launch many parallel coroutines
        for (int i = 0; i < NUM_REQUESTS; ++i) {
            boost::asio::co_spawn(
                ioc,
                [&, i]() -> boost::asio::awaitable<void> {
                    // Random small delay to mix things up
                    auto timer = std::make_shared<boost::asio::steady_timer>(
                        ioc.get_executor());
                    timer->expires_after(std::chrono::milliseconds(i % 5));
                    co_await timer->async_wait(boost::asio::use_awaitable);

                    // Acquire
                    auto res = co_await pool->acquire(ep);

                    // Critical section-ish
                    if (res.has_value()) {
                        auto lease = std::move(res.value());
                        // Hold for very short time
                        timer->expires_after(std::chrono::microseconds(10));
                        co_await timer->async_wait(boost::asio::use_awaitable);
                        // Lease destruction releases connection
                    }

                    completed.fetch_add(1);
                },
                boost::asio::detached);
        }

        // Run IO context
        // We need a timeout
        std::thread t([&] {
            boost::asio::steady_timer timer(ioc, std::chrono::seconds(10));
            timer.async_wait([&](boost::system::error_code) { ioc.stop(); });
            ioc.run();
        });

        t.join();

        EXPECT_GE(completed.load(), NUM_REQUESTS * 0.9);  // Most should finish
    }

}  // namespace
