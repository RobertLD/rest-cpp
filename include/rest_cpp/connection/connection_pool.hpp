#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>

#include "rest_cpp/config.hpp"
#include "rest_cpp/connection/connection.hpp"
#include "rest_cpp/connection/connection_pool_types.hpp"
#include "rest_cpp/endpoint.hpp"
#include "rest_cpp/error.hpp"
#include "rest_cpp/result.hpp"

namespace rest_cpp {

    /**
     * Thread-safe connection pool for async HTTP/HTTPS connections.
     *
     * SAFETY:
     * - All public methods are thread-safe and can be called from any thread
     * - Internal state protected by mutex, no strict strand requirement
     * - Coroutines may resume on different threads
     *
     * INVARIANTS:
     * 1. For each bucket: endpoint_total == in_use.size() + idle.size()
     * 2. Global: total_in_use_ == sum(bucket.in_use.size())
     * 3. No connection exists in both idle and in_use
     * 4. All idle connections pass health checks (when health checking enabled)
     * 5. Every waiter is either actively waiting or being removed by its owner
     * 6. All in_use connections have valid, non-null pointers
     *
     * ERRORS:
     * - Timeout: Resource temporarily unavailable, retry may succeed later
     * - Shutdown: Pool permanently closed, all future operations will fail
     * - InternalError: Unexpected failure (timer error, etc.), should be rare
     * - CircuitOpen: Circuit breaker open for endpoint, retry after timeout
     *
     * LIFECYCLE:
     * 1. Construction: Pool is alive and ready
     * 2. Operation: acquire() and release() work normally
     * 3. shutdown(): Marks pool as shutting down, cancels all waiters
     * 4. drain(): Optionally wait for in-use connections to be returned
     * 5. Destruction: Calls shutdown(), closes connections if configured
     */

    class ConnectionPool {
       public:
        using Conn = Connection<Mode::Async>;

        class Lease {
           public:
            Lease() = default;

            Lease(Lease&& other) noexcept { move_from(std::move(other)); }

            /// @brief Move lease from another
            Lease& operator=(Lease&& other) noexcept {
                if (this != &other) {
                    reset();
                    move_from(std::move(other));
                }
                return *this;
            }

            Lease(Lease const&) = delete;
            Lease& operator=(Lease const&) = delete;

            ~Lease() { reset(); }

            Conn* operator->() const noexcept { return get(); }

            Conn& operator*() const { return *get(); }

            /// @brief Get the underlying connection, or nullptr if inert
            Conn* get() const noexcept {
                auto st = state_.lock();
                if (!st || !st->alive.load(std::memory_order_acquire))
                    return nullptr;
                return conn_;
            }

            explicit operator bool() const noexcept { return get() != nullptr; }

            Endpoint const& endpoint() const noexcept { return endpoint_; }

            std::uint64_t id() const noexcept { return id_; }

           private:
            friend class ConnectionPool;

            /// @brief Internal state shared with the pool
            /// @note Used to detect pool shutdown
            struct State {
                std::atomic<bool> alive{true};
            };

            Lease(std::weak_ptr<State> st, Conn* c, Endpoint ep,
                  std::uint64_t id,
                  std::function<void(Endpoint const&, std::uint64_t)> ret)
                : state_(std::move(st)),
                  conn_(c),
                  endpoint_(std::move(ep)),
                  id_(id),
                  return_to_pool_(std::move(ret)) {}

            /// @brief Return the connection to the pool if still valid
            /// @note Called on destruction or move
            void reset() noexcept {
                auto st = state_.lock();
                if (!conn_) return;

                // If pool is already dead, do not call back into it.
                if (!st || !st->alive.load(std::memory_order_acquire)) {
                    conn_ = nullptr;
                    return;
                }
                // Otherwise, return that bitch
                if (return_to_pool_) return_to_pool_(endpoint_, id_);
                conn_ = nullptr;
            }

            /// @brief Move state from another lease

            void move_from(Lease&& other) noexcept {
                state_ = std::move(other.state_);
                conn_ = other.conn_;
                endpoint_ = std::move(other.endpoint_);
                id_ = other.id_;
                return_to_pool_ = std::move(other.return_to_pool_);
                other.conn_ = nullptr;
                other.id_ = 0;
            }

            std::weak_ptr<State> state_;
            Conn* conn_{nullptr};
            Endpoint endpoint_{};
            std::uint64_t id_{0};
            std::function<void(Endpoint const&, std::uint64_t)> return_to_pool_;
        };

        ConnectionPool(boost::asio::any_io_executor ex,
                       boost::asio::ssl::context& ssl_ctx,
                       AsyncConnectionPoolConfiguration cfg)
            : ex_(std::move(ex)),
              ssl_ctx_(ssl_ctx),
              cfg_(cfg),
              state_(std::make_shared<typename Lease::State>()) {}

        ~ConnectionPool() {
            // Signal shutdown
            state_->alive.store(false, std::memory_order_release);

            // Cancel all waiters
            std::list<Waiter> to_cancel;
            {
                std::lock_guard<std::mutex> lk(mu_);
                to_cancel.swap(waiters_);
            }

            for (auto& w : to_cancel) {
                if (w.timer) w.timer->cancel();
            }

            if (!cfg_.close_on_shutdown) return;

            // Close all idle and in-use connections
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [_, bucket] : buckets_) {
                for (auto& e : bucket.idle) {
                    if (e.conn) {
                        e.conn->close_http();
                        e.conn->close_https();
                    }
                }
                for (auto& [__, up] : bucket.in_use) {
                    if (up) {
                        up->close_http();
                        up->close_https();
                    }
                }
            }
        }

        /// @brief Try to acquire a connection immediately, returns nullopt if
        /// none
        /// @note Non-blocking, does not wait
        std::optional<Lease> try_acquire(Endpoint ep) {
            normalize_(ep);
            std::lock_guard<std::mutex> lk(mu_);
            return try_acquire_locked_(ep);
        }

        /// @brief Asynchronously acquire a connection lease
        /// @param ep Endpoint to connect to
        /// @param timeout Maximum time to wait for a connection
        /// @return Result containing Lease on success or AcquireError on
        /// failure
        /// @note Coroutine that may suspend
        boost::asio::awaitable<Result<Lease>> acquire(
            Endpoint ep, std::chrono::steady_clock::duration timeout =
                             std::chrono::steady_clock::duration::max()) {
            normalize_(ep);

            for (;;) {
                // Fast path: try without allocating a waiter
                if (auto l = try_acquire(ep)) {
                    metrics_.acquire_success.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::ok(std::move(*l));
                }

                // Check if we are shutting down
                if (!state_->alive.load(std::memory_order_acquire)) {
                    metrics_.acquire_shutdown.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::err(
                        Error{Error::Code::Unknown, "Pool is shutting down"});
                }

                auto t = std::make_shared<boost::asio::steady_timer>(ex_);

                // Handle infinite timeout
                if (timeout == std::chrono::steady_clock::duration::max()) {
                    t->expires_at(std::chrono::steady_clock::time_point::max());
                } else {
                    t->expires_after(timeout);
                }

                std::list<Waiter>::iterator my_waiter_it;
                {
                    std::lock_guard<std::mutex> lk(mu_);

                    // Re-check shutdown under lock
                    if (!state_->alive.load(std::memory_order_acquire)) {
                        metrics_.acquire_shutdown.fetch_add(
                            1, std::memory_order_relaxed);
                        co_return Result<Lease>::err(Error{
                            Error::Code::Unknown, "Pool is shutting down"});
                    }

                    // Determine wait reason and enqueue waiter
                    WaitReason reason = determine_wait_reason_locked_(ep);

                    // Enqueue into primary list
                    my_waiter_it = waiters_.emplace(
                        waiters_.end(), Waiter{ep, reason, t, true, {}});

                    // Enqueue into secondary list
                    if (reason == WaitReason::EndpointCapacity) {
                        auto& b = buckets_[ep];
                        b.waiters.push_back(&(*my_waiter_it));
                        my_waiter_it->queue_it = std::prev(b.waiters.end());
                    } else {
                        global_waiters_.push_back(&(*my_waiter_it));
                        my_waiter_it->queue_it =
                            std::prev(global_waiters_.end());
                    }

                    metrics_.waiters_total.fetch_add(1,
                                                     std::memory_order_relaxed);

                    // Close lost-wakeup window
                    if (auto l2 = try_acquire_locked_(ep)) {
                        // Remove from queues if we acquired immediately
                        if (reason == WaitReason::EndpointCapacity) {
                            buckets_[ep].waiters.erase(my_waiter_it->queue_it);
                        } else {
                            global_waiters_.erase(my_waiter_it->queue_it);
                        }

                        waiters_.erase(my_waiter_it);
                        metrics_.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                        metrics_.acquire_success.fetch_add(
                            1, std::memory_order_relaxed);
                        co_return Result<Lease>::ok(std::move(*l2));
                    }
                }

                boost::system::error_code ec;
                co_await t->async_wait(boost::asio::redirect_error(
                    boost::asio::use_awaitable, ec));

                // Remove ourselves on ALL exit paths
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    // ALWAYS erase the waiter, regardless of active status
                    // The waiter is ours, and we must remove it to keep the
                    // list clean

                    // If we are still active (timeout/cancel), we also need to
                    // remove from the secondary queue because pop_waiter didn't
                    // do it.
                    if (my_waiter_it->active) {
                        try {
                            if (my_waiter_it->reason ==
                                WaitReason::EndpointCapacity) {
                                // Important: Check if bucket still exists?
                                // Buckets are usually only created, rarely
                                // destroyed from map.
                                auto it = buckets_.find(ep);
                                if (it != buckets_.end()) {
                                    it->second.waiters.erase(
                                        my_waiter_it->queue_it);
                                }
                            } else {
                                global_waiters_.erase(my_waiter_it->queue_it);
                            }
                        } catch (...) {
                            // Should not happen with valid iterators
                        }
                        metrics_.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                    }

                    waiters_.erase(my_waiter_it);
                }

                if (!ec) {
                    // Timer expired = timeout
                    metrics_.acquire_timeout.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::err(
                        Error{Error::Code::Timeout, "Acquire timeout"});
                }

                if (ec == boost::asio::error::operation_aborted) {
                    continue;
                }

                // Unexpected error
                metrics_.acquire_internal_error.fetch_add(
                    1, std::memory_order_relaxed);
                co_return Result<Lease>::err(Error{
                    Error::Code::Unknown, "Internal error: " + ec.message()});
            }
        }

        /// Shutdown the pool immediately, canceling all waiters
        void shutdown() {
            state_->alive.store(false, std::memory_order_release);

            // Cancel all waiters
            std::list<Waiter> to_cancel;
            {
                std::lock_guard<std::mutex> lk(mu_);
                to_cancel.swap(waiters_);
                metrics_.waiters_total.store(0, std::memory_order_relaxed);
            }

            for (auto& w : to_cancel) {
                if (w.timer) w.timer->cancel();
            }

            // Close connections if configured
            if (cfg_.close_on_shutdown) {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [_, bucket] : buckets_) {
                    for (auto& e : bucket.idle) {
                        if (e.conn) {
                            e.conn->close_http();
                            e.conn->close_https();
                        }
                    }
                }
            }
        }

        /// Wait for all in-use connections to be returned (graceful shutdown)
        boost::asio::awaitable<bool> drain(
            std::chrono::steady_clock::duration timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;

            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (total_in_use_ == 0) {
                        co_return true;  // All connections returned
                    }
                }

                // Wait a bit before checking again
                auto timer = std::make_shared<boost::asio::steady_timer>(ex_);
                timer->expires_after(std::chrono::milliseconds(100));
                co_await timer->async_wait(boost::asio::use_awaitable);
            }

            co_return false;  // Timeout, some connections still in use
        }

        ///@brief Access metrics for monitoring
        ConnectionPoolMetrics const& metrics() const { return metrics_; }

        ///@brief Report connection failure for circuit breaker (call from user
        /// code)
        void report_failure(Endpoint const& ep) {
            report_connection_failure(ep);
        }

        ///@brief Report connection success for circuit breaker (call from user
        /// code)
        void report_success(Endpoint const& ep) {
            report_connection_success(ep);
        }
        enum class WaitReason {
            EndpointCapacity,  ///< Blocked by per-endpoint limit
            GlobalCapacity     ///< Blocked by global pool limit
        };

        ///@brief Entry for an idle connection with health tracking
        struct IdleEntry {
            std::unique_ptr<Conn> conn;  ///< Idle connection
            std::chrono::steady_clock::time_point
                last_used;                                  ///< Last used time
            std::chrono::steady_clock::time_point created;  ///< Creation time
            std::size_t reuse_count{0};  ///< Number of times reused
        };

        struct Waiter;

        ///@brief Per-endpoint bucket with circuit breaker
        struct Bucket {
            std::deque<IdleEntry> idle;  ///< Idle connections
            std::unordered_map<std::uint64_t, std::unique_ptr<Conn>>
                in_use;  ///< In-use connections

            /// @brief Waiters specifically waiting for this bucket
            /// @note Secondary index into waiters_ list
            std::list<Waiter*> waiters;

            // Circuit breaker state
            std::size_t consecutive_failures{
                0};  ///< Consecutive connection failures
            std::chrono::steady_clock::time_point
                circuit_open_until{};  ///< Circuit open duration

            ///@brief Check if circuit breaker is open
            bool is_circuit_open(
                std::chrono::steady_clock::time_point now) const {
                return now < circuit_open_until;
            }
        };

        /// @brief Waiter for connection availability
        struct Waiter {
            std::optional<Endpoint>
                ep;             ///< Endpoint waiting for (nullopt = any)
            WaitReason reason;  ///< Reason for waiting
            std::shared_ptr<boost::asio::steady_timer> timer;  ///< Timer for
                                                               /// timeout
            bool active{true};  ///< Whether still waiting

            /// @brief Iterator into the secondary queue (Bucket::waiters or
            /// ConnectionPool::global_waiters_)
            /// @note Used for O(1) removal on timeout/cancel
            std::list<Waiter*>::iterator queue_it;
        };

        /// @brief Normalize endpoint (default port, lowercase host)
        /// @param ep Endpoint to normalize
        /// @note Modifies the endpoint in place
        /// @note Called before any public method
        /// @note Super basic, just lowercases everything
        void normalize_(Endpoint& ep) {
            ep.normalize_default_port();
            ep.normalize_host();
        }

        /// @brief Check internal invariants, only in debug builds
        /// @note THis is just here because async code is so fucking hard to
        /// write
        void check_invariants_locked_() const {
#ifndef NDEBUG
            std::size_t computed_total = 0;

            for (auto const& [ep, b] : buckets_) {
                computed_total += b.in_use.size();

                // Invariant: no connection in both idle and in_use
                std::unordered_set<Conn const*> in_use_ptrs;
                for (auto const& [id, up] : b.in_use) {
                    assert(up && "in_use connection is null");
                    in_use_ptrs.insert(up.get());
                }

                for (auto const& entry : b.idle) {
                    assert(entry.conn && "idle connection is null");
                    assert(in_use_ptrs.find(entry.conn.get()) ==
                               in_use_ptrs.end() &&
                           "connection in both idle and in_use");
                }
            }

            assert(computed_total == total_in_use_ && "total_in_use_ drift");
#else
            return;
#endif
        }

        /// @brief Determine wait reason for an endpoint under lock
        WaitReason determine_wait_reason_locked_(Endpoint const& ep) const {
            auto it = buckets_.find(ep);
            if (it == buckets_.end()) {
                // New endpoint, check global capacity
                std::size_t total_total = total_in_use_ + total_idle_locked_();
                return (total_total >= cfg_.max_total_connections)
                           ? WaitReason::GlobalCapacity
                           : WaitReason::EndpointCapacity;
            }

            auto const& b = it->second;
            std::size_t ep_total = b.in_use.size() + b.idle.size();

            if (ep_total >= cfg_.max_connections_per_endpoint) {
                return WaitReason::EndpointCapacity;
            }
            return WaitReason::GlobalCapacity;
        }

        /// @brief Pop a waiter for the given endpoint under lock
        std::shared_ptr<boost::asio::steady_timer>
        pop_waiter_for_endpoint_locked_(Endpoint const& ep) {
            // First: try endpoint-specific waiters (O(1))
            auto it = buckets_.find(ep);
            if (it != buckets_.end()) {
                auto& b = it->second;
                while (!b.waiters.empty()) {
                    Waiter* w = b.waiters.front();
                    b.waiters.pop_front();  // Remove from secondary queue

                    if (w->active) {
                        w->active = false;
                        auto timer = w->timer;
                        metrics_.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                        return timer;
                    }
                }
            }

            // Second: try "any endpoint" / global waiters (O(1))
            while (!global_waiters_.empty()) {
                Waiter* w = global_waiters_.front();
                global_waiters_.pop_front();  // Remove from secondary queue

                if (w->active) {
                    w->active = false;
                    auto timer = w->timer;
                    metrics_.waiters_total.fetch_sub(1,
                                                     std::memory_order_relaxed);
                    return timer;
                }
            }

            return {};
        }

        /// @brief Compute total number of idle connections under lock
        std::size_t total_idle_locked_() const {
            std::size_t n = 0;
            for (auto const& [_, b] : buckets_) n += b.idle.size();
            return n;
        }

        /// @brief Prune idle connections that have expired under lock
        void prune_idle_locked_(std::chrono::steady_clock::time_point now) {
            if (cfg_.connection_idle_ttl.count() <= 0) return;

            for (auto& [_, b] : buckets_) {
                while (!b.idle.empty()) {
                    auto const& front = b.idle.front();
                    if (now - front.last_used < cfg_.connection_idle_ttl) break;

                    if (cfg_.close_on_prune && front.conn) {
                        front.conn->close_http();
                        front.conn->close_https();
                    }
                    b.idle.pop_front();
                    metrics_.connection_pruned.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }

        /// @brief Report connection failure for circuit breaker
        /// @param ep Endpoint of the failed connection
        void report_connection_failure(Endpoint const& ep) {
            std::lock_guard<std::mutex> lk(mu_);
            auto& b = buckets_[ep];

            b.consecutive_failures++;

            if (b.consecutive_failures >=
                cfg_.circuit_breaker_failure_threshold) {
                b.circuit_open_until = std::chrono::steady_clock::now() +
                                       cfg_.circuit_breaker_timeout;
                metrics_.circuit_breaker_opened.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        /// @brief Report connection success for circuit breaker
        /// @param ep Endpoint of the successful connection
        void report_connection_success(Endpoint const& ep) {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = buckets_.find(ep);
            if (it == buckets_.end()) return;

            auto& b = it->second;
            if (b.consecutive_failures > 0) {
                b.consecutive_failures = 0;
                metrics_.circuit_breaker_closed.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        /// @brief Basic health check for a connection
        bool is_connection_healthy_(
            [[maybe_unused]] Conn const& conn) const noexcept {
            // Basic health check - can be enhanced with actual socket checks
            // For now, just return true as Connection class will be enhanced
            // separately
            return true;
        }

        /// @brief Release a connection back to the pool
        void release(Endpoint const& ep, std::uint64_t id) noexcept {
            std::shared_ptr<boost::asio::steady_timer> w;

            try {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = buckets_.find(ep);

                if (it == buckets_.end()) {
                    metrics_.release_invalid_id.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                auto& b = it->second;
                auto it2 = b.in_use.find(id);

                if (it2 == b.in_use.end()) {
                    metrics_.release_invalid_id.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                auto up = std::move(it2->second);
                b.in_use.erase(it2);
                --total_in_use_;
                metrics_.total_in_use.store(total_in_use_,
                                            std::memory_order_relaxed);

                // Health check before recycling
                bool healthy = up && is_connection_healthy_(*up);

                if (healthy) {
                    auto now = std::chrono::steady_clock::now();
                    b.idle.push_back(IdleEntry{
                        std::move(up), now,
                        now,  // created time (will be set properly on first
                              // create)
                        0     // reuse_count
                    });
                    metrics_.total_idle.fetch_add(1, std::memory_order_relaxed);
                } else {
                    metrics_.connection_dropped_unhealthy.fetch_add(
                        1, std::memory_order_relaxed);
                    // Connection dropped, capacity freed
                }

                // Pop waiter UNDER LOCK, but don't cancel  just yet
                w = pop_waiter_for_endpoint_locked_(ep);

                check_invariants_locked_();
            } catch (...) {
                // Swallow all exceptions in release to avoid throwing from
                // destructor
            }

            // Cancel OUTSIDE lock to avoid deadlock
            if (w) {
                w->cancel();
            }
        }

        /// @brief Try to acquire a connection under lock
        std::optional<Lease> try_acquire_locked_(Endpoint const& ep) {
            // Check shutdown
            if (!state_->alive.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            auto now = std::chrono::steady_clock::now();
            prune_idle_locked_(now);

            auto& b = buckets_[ep];

            // Check circuit breaker
            if (b.is_circuit_open(now)) {
                return std::nullopt;
            }

            // Prefer reusing idle connections
            while (!b.idle.empty()) {
                auto entry = std::move(b.idle.front());
                b.idle.pop_front();
                metrics_.total_idle.fetch_sub(1, std::memory_order_relaxed);

                // Validate health
                if (!entry.conn || !is_connection_healthy_(*entry.conn)) {
                    metrics_.connection_dropped_unhealthy.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;  // Try next idle connection
                }

                // Check reuse limit
                if (entry.reuse_count >= cfg_.max_connection_reuse_count) {
                    metrics_.connection_dropped_reuse_limit.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }

                // Check age limit
                if (now - entry.created > cfg_.max_connection_age) {
                    metrics_.connection_dropped_age_limit.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }

                // Connection is good to reuse
                entry.reuse_count++;
                auto id = next_id_++;
                Conn* raw = entry.conn.get();
                b.in_use.emplace(id, std::move(entry.conn));
                ++total_in_use_;

                metrics_.total_in_use.store(total_in_use_,
                                            std::memory_order_relaxed);
                metrics_.connection_reused.fetch_add(1,
                                                     std::memory_order_relaxed);

                check_invariants_locked_();
                return Lease(state_, raw, ep, id,
                             [this](Endpoint const& e, std::uint64_t id_) {
                                 release(e, id_);
                             });
            }

            // No idle connections available, check capacity for creating new
            const std::size_t endpoint_total = b.in_use.size() + b.idle.size();
            const std::size_t total_total =
                total_in_use_ + total_idle_locked_();

            if (endpoint_total >= cfg_.max_connections_per_endpoint)
                return std::nullopt;
            if (total_total >= cfg_.max_total_connections) return std::nullopt;

            // Create a new connection and mark it in-use
            auto up = std::make_unique<Conn>(ex_, ssl_ctx_, ep);
            Conn* raw = up.get();
            auto id = next_id_++;

            b.in_use.emplace(id, std::move(up));
            ++total_in_use_;

            metrics_.total_in_use.store(total_in_use_,
                                        std::memory_order_relaxed);
            metrics_.connection_created.fetch_add(1, std::memory_order_relaxed);

            check_invariants_locked_();
            return Lease(state_, raw, ep, id,
                         [this](Endpoint const& e, std::uint64_t id_) {
                             release(e, id_);
                         });
        }

        boost::asio::any_io_executor ex_;     ///< Executor for async operations
        boost::asio::ssl::context& ssl_ctx_;  ///< SSL context for HTTPS
        AsyncConnectionPoolConfiguration cfg_;  ///< Pool configuration

        mutable std::mutex mu_;  ///< Mutex for protecting internal state
        std::unordered_map<Endpoint, Bucket>
            buckets_;  ///< Per-endpoint buckets

        std::list<Waiter> waiters_;  ///< Stable iterators for removal
        std::list<Waiter*>
            global_waiters_;  ///< Waiters waiting on global capacity

        std::size_t total_in_use_{0};  ///< Total in-use connections
        std::uint64_t next_id_{1};     ///< Next connection ID

        std::shared_ptr<typename Lease::State> state_;  ///< Shared pool state
        ConnectionPoolMetrics metrics_;  ///< Metrics for monitoring
    };

}  // namespace rest_cpp
