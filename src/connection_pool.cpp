// connection_pool.cpp (key diffs only; integrate into your existing .cpp)

#include "rest_cpp/connection_pool.hpp"

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <limits>

namespace rest_cpp {

    using awaitable_lease = boost::asio::awaitable<AsyncConnectionPool::Lease>;
    using awaitable_void = boost::asio::awaitable<void>;

    AsyncConnectionPool::AsyncConnectionPool(
        executor_type ex, boost::asio::ssl::context& ssl_ctx,
        AsyncConnectionPoolConfiguration opt)
        : m_state(std::make_shared<State>(std::move(ex), ssl_ctx,
                                          std::move(opt))) {}

    AsyncConnectionPool::~AsyncConnectionPool() {
        auto s = std::move(m_state);
        if (!s) return;

        // Best-effort synchronous drain on the strand.
        boost::asio::dispatch(s->strand, [s] {
            s->shutting_down = true;

            for (auto& w : s->waiters) {
                if (!w) continue;
                w->notified = true;
                w->timer.cancel();
            }
            s->waiters.clear();

            for (auto& [ep, st] : s->by_endpoint) {
                (void)ep;
                while (!st.idle.empty()) {
                    auto entry = std::move(st.idle.front());
                    st.idle.pop_front();
                    close_connection(entry.conn);
                }
                st.open = 0;
                st.in_use = 0;
            }

            s->by_endpoint.clear();
            s->total_open = 0;
            s->total_in_use = 0;
        });

        // After destructor returns, Leases that call reset() will see expired
        // weak_ptr.
    }

    awaitable_lease AsyncConnectionPool::acquire(const UrlComponents& url) {
        co_return co_await acquire(endpoint_from_url(url));
    }

    awaitable_lease AsyncConnectionPool::acquire(const Endpoint& ep) {
        Endpoint normalized = ep;
        normalized.normalize_default_port();
        normalized.normalize_host();

        auto s = m_state;
        if (!s) co_return Lease{};

        co_return co_await boost::asio::co_spawn(
            s->strand,
            [s, normalized]() -> boost::asio::awaitable<Lease> {
                co_return co_await acquire_impl(s, normalized);
            },
            boost::asio::use_awaitable);
    }

    awaitable_void AsyncConnectionPool::reap_idle() {
        auto s = m_state;
        if (!s) co_return;

        co_await boost::asio::co_spawn(
            s->strand,
            [s]() -> boost::asio::awaitable<void> {
                if (s->shutting_down) co_return;

                const auto now = clock_type::now();
                for (auto it = s->by_endpoint.begin();
                     it != s->by_endpoint.end();) {
                    prune_idle_for_endpoint(*s, it->second, it->first, now);
                    if (it->second.open == 0)
                        it = s->by_endpoint.erase(it);
                    else
                        ++it;
                }
                co_return;
            },
            boost::asio::use_awaitable);
    }

    awaitable_void AsyncConnectionPool::shutdown() {
        auto s = m_state;
        if (!s) co_return;

        co_await boost::asio::co_spawn(
            s->strand,
            [s]() -> boost::asio::awaitable<void> {
                if (s->shutting_down) co_return;
                s->shutting_down = true;

                for (auto& w : s->waiters) {
                    if (!w) continue;
                    w->notified = true;
                    w->timer.cancel();
                }
                s->waiters.clear();

                if (!s->opt.close_connections_on_shutdown) co_return;

                for (auto& [ep, st] : s->by_endpoint) {
                    (void)ep;
                    while (!st.idle.empty()) {
                        auto entry = std::move(st.idle.front());
                        st.idle.pop_front();
                        close_connection(entry.conn);
                        if (st.open) --st.open;
                        if (s->total_open) --s->total_open;
                    }
                }
                co_return;
            },
            boost::asio::use_awaitable);
    }

    boost::asio::awaitable<AsyncConnectionPool::Stats>
    AsyncConnectionPool::stats() {
        auto s = m_state;
        if (!s) co_return Stats{};

        co_return co_await boost::asio::co_spawn(
            s->strand,
            [s]() -> boost::asio::awaitable<Stats> {
                Stats out{};
                out.total_open = s->total_open;

                std::size_t total_idle = 0;
                for (auto const& [ep, st] : s->by_endpoint) {
                    (void)ep;
                    total_idle += st.idle.size();
                }
                out.total_idle = total_idle;
                out.total_in_use = s->total_in_use;
                co_return out;
            },
            boost::asio::use_awaitable);
    }

    // -------------------------
    // return path (now static, state-based)
    // -------------------------

    void AsyncConnectionPool::return_to_pool(std::shared_ptr<State> s,
                                             Endpoint ep, connection_ptr conn,
                                             bool reusable) noexcept {
        if (!s || !conn) return;

        boost::asio::post(s->strand, [s = std::move(s), ep = std::move(ep),
                                      conn = std::move(conn), reusable] {
            return_to_pool_on_strand(std::move(s), std::move(ep),
                                     std::move(conn), reusable);
        });
    }

    void AsyncConnectionPool::return_to_pool_on_strand(std::shared_ptr<State> s,
                                                       Endpoint ep,
                                                       connection_ptr conn,
                                                       bool reusable) {
        if (!s || !conn) return;

        auto it = s->by_endpoint.find(ep);
        if (it == s->by_endpoint.end()) reusable = false;

        EndpointState* st =
            (it == s->by_endpoint.end()) ? nullptr : &it->second;

        if (st && st->in_use) --st->in_use;
        if (s->total_in_use) --s->total_in_use;

        const bool should_close =
            (!reusable) || s->shutting_down || (st == nullptr) ||
            (st && st->idle.size() >= s->opt.max_connections_per_endpoint);

        if (should_close) {
            close_connection(conn);

            if (st && st->open) --st->open;
            if (s->total_open) --s->total_open;

            if (st && st->open == 0) s->by_endpoint.erase(ep);
        } else {
            st->idle.push_back(IdleEntry{std::move(conn), clock_type::now()});
        }

        if (!s->waiters.empty()) {
            auto w = std::move(s->waiters.front());
            s->waiters.pop_front();
            if (w) {
                w->notified = true;
                w->timer.cancel();
            }
        }
    }

    // -------------------------
    // acquire_impl now takes state
    // -------------------------

    boost::asio::awaitable<AsyncConnectionPool::Lease>
    AsyncConnectionPool::acquire_impl(std::shared_ptr<State> s, Endpoint ep) {
        if (!s || s->shutting_down) co_return Lease{};

        auto& st = s->by_endpoint[ep];

        prune_idle_for_endpoint(*s, st, ep, clock_type::now());

        if (!st.idle.empty()) {
            auto entry = std::move(st.idle.front());
            st.idle.pop_front();
            ++st.in_use;
            ++s->total_in_use;
            co_return Lease{std::weak_ptr<State>(s), std::move(ep),
                            std::move(entry.conn)};
        }

        const bool total_ok = (s->total_open < s->opt.max_total_connections);
        const bool per_ep_ok = (st.open < s->opt.max_connections_per_endpoint);

        if (total_ok && per_ep_ok) {
            auto conn = std::make_shared<connection_type>(s->ex, *s->ssl_ctx);
            ++st.open;
            ++s->total_open;

            ++st.in_use;
            ++s->total_in_use;

            co_return Lease{std::weak_ptr<State>(s), std::move(ep),
                            std::move(conn)};
        }

        auto ex = co_await boost::asio::this_coro::executor;
        boost::asio::cancellation_state cs =
            co_await boost::asio::this_coro::cancellation_state;

        auto waiter = std::make_shared<Waiter>(ex);
        s->waiters.push_back(waiter);

        waiter->timer.expires_at(clock_type::time_point::max());

        boost::system::error_code ec;
        co_await waiter->timer.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (cs.cancelled() != boost::asio::cancellation_type::none) {
            co_return Lease{};
        }
        if (s->shutting_down) co_return Lease{};

        for (;;) {
            prune_idle_for_endpoint(*s, st, ep, clock_type::now());

            if (!st.idle.empty()) {
                auto entry = std::move(st.idle.front());
                st.idle.pop_front();
                ++st.in_use;
                ++s->total_in_use;
                co_return Lease{std::weak_ptr<State>(s), std::move(ep),
                                std::move(entry.conn)};
            }

            const bool total_ok2 =
                (s->total_open < s->opt.max_total_connections);
            const bool per_ep_ok2 =
                (st.open < s->opt.max_connections_per_endpoint);

            if (total_ok2 && per_ep_ok2) {
                auto conn =
                    std::make_shared<connection_type>(s->ex, *s->ssl_ctx);
                ++st.open;
                ++s->total_open;

                ++st.in_use;
                ++s->total_in_use;

                co_return Lease{std::weak_ptr<State>(s), std::move(ep),
                                std::move(conn)};
            }

            auto waiter2 = std::make_shared<Waiter>(ex);
            s->waiters.push_back(waiter2);
            waiter2->timer.expires_at(clock_type::time_point::max());
            co_await waiter2->timer.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (cs.cancelled() != boost::asio::cancellation_type::none) {
                co_return Lease{};
            }
            if (s->shutting_down) co_return Lease{};
        }
    }

    // -------------------------
    // utilities
    // -------------------------

    void AsyncConnectionPool::close_connection(connection_ptr& c) noexcept {
        if (!c) return;
        try {
            c->close_http();
            c->close_https();
        } catch (...) {
        }
        c.reset();
    }

    void AsyncConnectionPool::prune_idle_for_endpoint(
        State& s, EndpointState& st, const Endpoint& ep,
        clock_type::time_point now) {
        (void)s;
        (void)ep;

        if (s.opt.connection_idle_ttl.count() <= 0) return;

        const auto ttl = s.opt.connection_idle_ttl;
        while (!st.idle.empty()) {
            const auto& front = st.idle.front();
            const auto age = now - front.last_used;
            if (age <= ttl) break;

            auto entry = std::move(st.idle.front());
            st.idle.pop_front();
            close_connection(entry.conn);

            if (st.open) --st.open;
            if (s.total_open) --s.total_open;
        }
    }

    Endpoint AsyncConnectionPool::endpoint_from_url(const UrlComponents& u) {
        Endpoint ep;
        ep.host = u.host;
        ep.port = u.port;
        ep.https = u.https;
        ep.normalize_default_port();
        ep.normalize_host();
        return ep;
    }

}  // namespace rest_cpp
