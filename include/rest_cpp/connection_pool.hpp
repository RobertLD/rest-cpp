// connection_pool.hpp (minimal-diff hardened version)

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>

#include "connection.hpp"
#include "endpoint.hpp"
#include "rest_cpp/config.hpp"
#include "url.hpp"

namespace rest_cpp {

    class AsyncConnectionPool {
       public:
        using executor_type = boost::asio::any_io_executor;
        using clock_type = std::chrono::steady_clock;

        using connection_type = Connection<Mode::Async>;
        using connection_ptr = std::shared_ptr<connection_type>;

       private:
        struct IdleEntry {
            connection_ptr conn;
            clock_type::time_point last_used{};
        };

        struct EndpointState {
            std::deque<IdleEntry> idle;
            std::size_t open = 0;
            std::size_t in_use = 0;
        };

        struct Waiter {
            boost::asio::steady_timer timer;
            bool notified = false;

            explicit Waiter(const executor_type& ex) : timer(ex) {}
        };

        // NEW: shared state owned by the pool, referenced weakly by leases.
        struct State {
            executor_type ex;
            boost::asio::strand<executor_type> strand;
            boost::asio::ssl::context* ssl_ctx = nullptr;
            AsyncConnectionPoolConfiguration opt{};

            bool shutting_down = false;

            std::unordered_map<Endpoint, EndpointState> by_endpoint;
            std::size_t total_open = 0;
            std::size_t total_in_use = 0;
            std::deque<std::shared_ptr<Waiter>> waiters;

            State(executor_type ex_, boost::asio::ssl::context& ctx,
                  AsyncConnectionPoolConfiguration opt_)
                : ex(std::move(ex_)),
                  strand(boost::asio::make_strand(ex)),
                  ssl_ctx(&ctx),
                  opt(std::move(opt_)) {}
        };

       public:
        class Lease {
           public:
            Lease() = default;

            ~Lease() { reset(); }

            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;

            Lease(Lease&& other) noexcept
                : m_state(std::exchange(other.m_state, {})),
                  m_endpoint(std::move(other.m_endpoint)),
                  m_conn(std::move(other.m_conn)),
                  m_reusable(std::exchange(other.m_reusable, false)) {}

            Lease& operator=(Lease&& other) noexcept {
                if (this != &other) {
                    reset();
                    m_state = std::exchange(other.m_state, {});
                    m_endpoint = std::move(other.m_endpoint);
                    m_conn = std::move(other.m_conn);
                    m_reusable = std::exchange(other.m_reusable, false);
                }
                return *this;
            }

            connection_type* operator->() const noexcept {
                return m_conn.get();
            }

            connection_type& operator*() const noexcept { return *m_conn; }

            connection_ptr get() const noexcept { return m_conn; }

            explicit operator bool() const noexcept {
                return static_cast<bool>(m_conn);
            }

            void mark_bad() noexcept { m_reusable = false; }

            void reset() noexcept;

            Endpoint endpoint() const noexcept { return m_endpoint; }

           private:
            friend class AsyncConnectionPool;

            Lease(std::weak_ptr<State> st, Endpoint ep,
                  connection_ptr c) noexcept
                : m_state(std::move(st)),
                  m_endpoint(std::move(ep)),
                  m_conn(std::move(c)) {}

            std::weak_ptr<State> m_state;
            Endpoint m_endpoint{};
            connection_ptr m_conn{};
            bool m_reusable{true};
        };

        AsyncConnectionPool(executor_type ex,
                            boost::asio::ssl::context& ssl_ctx,
                            AsyncConnectionPoolConfiguration opt = {});

        AsyncConnectionPool(const AsyncConnectionPool&) = delete;
        AsyncConnectionPool& operator=(const AsyncConnectionPool&) = delete;

        ~AsyncConnectionPool();

        executor_type get_executor() const noexcept { return m_state->ex; }

        AsyncConnectionPoolConfiguration options() const noexcept {
            return m_state->opt;
        }

        boost::asio::awaitable<Lease> acquire(const UrlComponents& url);
        boost::asio::awaitable<Lease> acquire(const Endpoint& ep);

        boost::asio::awaitable<void> reap_idle();
        boost::asio::awaitable<void> shutdown();

        struct Stats {
            std::size_t total_open = 0;
            std::size_t total_idle = 0;
            std::size_t total_in_use = 0;
        };

        boost::asio::awaitable<Stats> stats();

       private:
        // Helpers now take State&
        static void close_connection(connection_ptr& c) noexcept;
        static void prune_idle_for_endpoint(State& s, EndpointState& st,
                                            const Endpoint& ep,
                                            clock_type::time_point now);

        static Endpoint endpoint_from_url(const UrlComponents& u);

        static boost::asio::awaitable<Lease> acquire_impl(
            std::shared_ptr<State> s, Endpoint ep);

        static void return_to_pool(std::shared_ptr<State> s, Endpoint ep,
                                   connection_ptr conn, bool reusable) noexcept;
        static void return_to_pool_on_strand(std::shared_ptr<State> s,
                                             Endpoint ep, connection_ptr conn,
                                             bool reusable);

       private:
        std::shared_ptr<State> m_state;
    };

    // -------------------------
    // Lease::reset hardened
    // -------------------------

    inline void AsyncConnectionPool::Lease::reset() noexcept {
        if (!m_conn) return;

        auto st = m_state.lock();
        if (!st) {
            // Pool already gone; just drop our ref and let Connection dtor
            // close.
            m_conn.reset();
            m_reusable = true;
            m_state.reset();
            m_endpoint.clear();
            return;
        }

        // Post return onto the pool strand (no raw pointer).
        AsyncConnectionPool::return_to_pool(std::move(st),
                                            std::move(m_endpoint),
                                            std::move(m_conn), m_reusable);

        m_reusable = true;
        m_state.reset();
        m_endpoint.clear();
    }

}  // namespace rest_cpp
