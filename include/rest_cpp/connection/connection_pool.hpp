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
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../config.hpp"
#include "../endpoint.hpp"
#include "../error.hpp"
#include "../result.hpp"
#include "./connection.hpp"
#include "./connection_pool_types.hpp"

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
     * 2. Global: m_total_in_use == sum(bucket.in_use.size())
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
            [[nodiscard]] Conn* get() const noexcept {
                auto state = m_state.lock();
                if (!state || !state->alive.load(std::memory_order_acquire)) {
                    return nullptr;
                }
                return m_conn;
            }

            explicit operator bool() const noexcept { return get() != nullptr; }

            [[nodiscard]] Endpoint const& endpoint() const noexcept {
                return endpoint_;
            }

            [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

           private:
            friend class ConnectionPool;

            /// @brief Internal state shared with the pool
            /// @note Used to detect pool shutdown
            struct State {
                std::atomic<bool> alive{true};
            };

            Lease(std::weak_ptr<State> state, Conn* conn, Endpoint endpoint,
                  std::uint64_t identifier,
                  std::function<void(Endpoint const&, std::uint64_t)> ret)
                : m_state(std::move(state)),
                  m_conn(conn),
                  endpoint_(std::move(endpoint)),
                  id_(identifier),
                  return_to_pool(std::move(ret)) {}

            /// @brief Return the connection to the pool if still valid
            /// @note Called on destruction or move
            void reset() noexcept {
                auto state = m_state.lock();
                if (m_conn == nullptr) {
                    return;
                }

                // If pool is already dead, do not call back into it.
                if (!state || !state->alive.load(std::memory_order_acquire)) {
                    m_conn = nullptr;
                    return;
                }
                // Otherwise, return that bitch
                if (return_to_pool) {
                    return_to_pool(endpoint_, id_);
                }
                m_conn = nullptr;
            }

            /// @brief Move state from another lease

            /// @brief Move state from another lease
            void move_from(
                Lease&&
                    other) noexcept  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
            {
                // Move all members from the source lease, ensuring proper move
                // semantics.
                m_state = std::move(other.m_state);
                m_conn = other.m_conn;
                endpoint_ = std::move(other.endpoint_);
                id_ = other.id_;
                return_to_pool = std::move(other.return_to_pool);
                // Invalidate the source lease to prevent double‑return.
                other.m_conn = nullptr;
                other.id_ = 0;
            }

            std::weak_ptr<State> m_state;
            Conn* m_conn{nullptr};
            Endpoint endpoint_{};
            std::uint64_t id_{0};
            std::function<void(Endpoint const&, std::uint64_t)> return_to_pool;
        };

        ConnectionPool(const ConnectionPool&) = delete;
        ConnectionPool(ConnectionPool&&) = delete;
        ConnectionPool& operator=(const ConnectionPool&) = delete;
        ConnectionPool& operator=(ConnectionPool&&) = delete;

        ConnectionPool(boost::asio::any_io_executor executor,
                       boost::asio::ssl::context& ssl_ctx,
                       AsyncConnectionPoolConfiguration cfg)
            : m_ex(std::move(executor)),
              m_ssl_ctx(ssl_ctx),
              m_cfg(cfg),
              m_state(std::make_shared<typename Lease::State>()) {}

        ~ConnectionPool() {
            // Signal shutdown
            m_state->alive.store(false, std::memory_order_release);

            // Cancel all waiters
            std::list<Waiter> to_cancel;
            {
                std::lock_guard<std::mutex> lock_guard(mu_);
                to_cancel.swap(m_waiters);
            }

            for (auto& pending_waiter : to_cancel) {
                if (pending_waiter.timer) {
                    pending_waiter.timer->cancel();
                }
            }

            if (!m_cfg.close_on_shutdown) {
                return;
            }

            // Close all idle and in-use connections
            std::lock_guard<std::mutex> lock_guard(mu_);
            for (auto& [_endpoint, bucket] : m_buckets) {
                for (auto& idle_entry : bucket.idle) {
                    if (idle_entry.conn) {
                        idle_entry.conn->close_http();
                        idle_entry.conn->close_https();
                    }
                }
                for (auto& [_id, connection_handle] : bucket.in_use) {
                    if (connection_handle) {
                        connection_handle->close_http();
                        connection_handle->close_https();
                    }
                }
            }
        }

        /// @brief Try to acquire a connection immediately, returns nullopt if
        /// none
        /// @note Non-blocking, does not wait
        std::optional<Lease> try_acquire(Endpoint endpoint) {
            normalize_endpoint(endpoint);
            std::lock_guard<std::mutex> lock(mu_);
            return try_acquire_locked(endpoint);
        }

        /// @brief Asynchronously acquire a connection lease
        /// @param ep Endpoint to connect to
        /// @param timeout Maximum time to wait for a connection
        /// @return Result containing Lease on success or AcquireError on
        /// failure
        /// @note Coroutine that may suspend
        boost::asio::awaitable<Result<Lease>> acquire(
            Endpoint endpoint, std::chrono::steady_clock::duration timeout =
                                   std::chrono::steady_clock::duration::max()) {
            normalize_endpoint(endpoint);

            for (;;) {
                // Fast path: try without allocating a waiter
                if (auto lease = try_acquire(endpoint)) {
                    m_metrics.acquire_success.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::ok(std::move(*lease));
                }

                // Check if we are shutting down
                if (!m_state->alive.load(std::memory_order_acquire)) {
                    m_metrics.acquire_shutdown.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::err(
                        Error{Error::Code::Unknown, "Pool is shutting down"});
                }

                auto timer = std::make_shared<boost::asio::steady_timer>(m_ex);

                timer->expires_after(timeout);

                std::list<Waiter>::iterator waiter_iterator;
                {
                    std::lock_guard<std::mutex> lock(mu_);

                    // Re-check shutdown under lock
                    if (!m_state->alive.load(std::memory_order_acquire)) {
                        m_metrics.acquire_shutdown.fetch_add(
                            1, std::memory_order_relaxed);
                        co_return Result<Lease>::err(Error{
                            Error::Code::Unknown, "Pool is shutting down"});
                    }

                    // Determine wait reason and enqueue waiter
                    WaitReason reason = determine_wait_reason_locked(endpoint);

                    // Enqueue into primary list
                    waiter_iterator = m_waiters.emplace(m_waiters.end(),
                                                        Waiter{.ep = endpoint,
                                                               .reason = reason,
                                                               .timer = timer,
                                                               .active = true,
                                                               .queue_it = {}});

                    // Enqueue into secondary list
                    if (reason == WaitReason::EndpointCapacity) {
                        auto& bucket = m_buckets[endpoint];
                        bucket.waiters.push_back(&(*waiter_iterator));
                        waiter_iterator->queue_it =
                            std::prev(bucket.waiters.end());
                    } else {
                        m_global_waiters.push_back(&(*waiter_iterator));
                        waiter_iterator->queue_it =
                            std::prev(m_global_waiters.end());
                    }

                    m_metrics.waiters_total.fetch_add(
                        1, std::memory_order_relaxed);

                    // Close lost-wakeup window
                    if (auto lock_con_2 = try_acquire_locked(endpoint)) {
                        // Remove from queues if we acquired immediately
                        if (reason == WaitReason::EndpointCapacity) {
                            m_buckets[endpoint].waiters.erase(
                                waiter_iterator->queue_it);
                        } else {
                            m_global_waiters.erase(waiter_iterator->queue_it);
                        }

                        m_waiters.erase(waiter_iterator);
                        m_metrics.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                        m_metrics.acquire_success.fetch_add(
                            1, std::memory_order_relaxed);
                        co_return Result<Lease>::ok(std::move(*lock_con_2));
                    }
                }

                boost::system::error_code errc;
                co_await timer->async_wait(boost::asio::redirect_error(
                    boost::asio::use_awaitable, errc));

                // Remove ourselves on ALL exit paths
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    // ALWAYS erase the waiter, regardless of active status
                    // The waiter is ours, and we must remove it to keep the
                    // list clean

                    // If we are still active (timeout/cancel), we also need to
                    // remove from the secondary queue because pop_waiter didn't
                    // do it.
                    if (waiter_iterator->active) {
                        try {
                            if (waiter_iterator->reason ==
                                WaitReason::EndpointCapacity) {
                                // Important: Check if bucket still exists?
                                // Buckets are usually only created, rarely
                                // destroyed from map.
                                auto bucket_iter = m_buckets.find(endpoint);
                                if (bucket_iter != m_buckets.end()) {
                                    bucket_iter->second.waiters.erase(
                                        waiter_iterator->queue_it);
                                }
                            } else {
                                m_global_waiters.erase(
                                    waiter_iterator->queue_it);
                            }
                            // NOLINTNEXTLINE(bugprone-empty-catch)
                        } catch (...) {
                            // Should not happen with valid iterators
                        }
                        m_metrics.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                    }

                    m_waiters.erase(waiter_iterator);
                }

                if (!errc) {
                    // Timer expired = timeout
                    m_metrics.acquire_timeout.fetch_add(
                        1, std::memory_order_relaxed);
                    co_return Result<Lease>::err(
                        Error{Error::Code::Timeout, "Acquire timeout"});
                }

                if (errc == boost::asio::error::operation_aborted) {
                    continue;
                }

                // Unexpected error
                m_metrics.acquire_internal_error.fetch_add(
                    1, std::memory_order_relaxed);
                co_return Result<Lease>::err(Error{
                    Error::Code::Unknown, "Internal error: " + errc.message()});
            }
        }

        /// Shutdown the pool immediately, canceling all waiters
        void shutdown() {
            m_state->alive.store(false, std::memory_order_release);

            // Cancel all waiters
            std::list<Waiter> to_cancel;
            {
                std::lock_guard<std::mutex> lock(mu_);
                to_cancel.swap(m_waiters);
                m_metrics.waiters_total.store(0, std::memory_order_relaxed);
            }

            for (auto& waiter : to_cancel) {
                if (waiter.timer) {
                    waiter.timer->cancel();
                }
            }

            // Close connections if configured
            if (m_cfg.close_on_shutdown) {
                std::lock_guard<std::mutex> lock(mu_);
                for (auto& [_endpoint, bucket] : m_buckets) {
                    for (auto& idle_entry : bucket.idle) {
                        if (idle_entry.conn) {
                            idle_entry.conn->close_http();
                            idle_entry.conn->close_https();
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
                    std::lock_guard<std::mutex> lock(mu_);
                    if (m_total_in_use == 0) {
                        co_return true;  // All connections returned
                    }
                }

                // Wait a bit before checking again
                auto timer = std::make_shared<boost::asio::steady_timer>(m_ex);
                timer->expires_after(std::chrono::milliseconds(100));
                co_await timer->async_wait(boost::asio::use_awaitable);
            }

            co_return false;  // Timeout, some connections still in use
        }

        ///@brief Access metrics for monitoring
        ConnectionPoolMetrics const& metrics() const { return m_metrics; }

        ///@brief Report connection failure for circuit breaker (call from user
        /// code)
        void report_failure(Endpoint const& endpoint) {
            report_connection_failure(endpoint);
        }

        ///@brief Report connection success for circuit breaker (call from user
        /// code)
        void report_success(Endpoint const& endpoint) {
            report_connection_success(endpoint);
        }
        enum class WaitReason : std::uint8_t {
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
            /// @note Secondary index into m_waiters list
            std::list<Waiter*> waiters;

            // Circuit breaker state
            std::size_t consecutive_failures{
                0};  ///< Consecutive connection failures
            std::chrono::steady_clock::time_point
                circuit_open_until;  ///< Circuit open duration

            ///@brief Check if circuit breaker is open
            [[nodiscard]] bool is_circuit_open(
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
            /// ConnectionPool::m_global_waiters)
            /// @note Used for O(1) removal on timeout/cancel
            std::list<Waiter*>::iterator queue_it;
        };

        /// @brief Normalize endpoint (default port, lowercase host)
        /// @param ep Endpoint to normalize
        /// @note Modifies the endpoint in place
        /// @note Called before any public method
        /// @note Super basic, just lowercases everything
        static void normalize_endpoint(Endpoint& endpoint) {
            endpoint.normalize_default_port();
            endpoint.normalize_host();
        }

        /// @brief Check internal invariants, only in debug builds
        /// @note THis is just here because async code is so fucking hard to
        /// write
        void check_invariants_locked() {
#ifndef NDEBUG
            std::size_t computed_total = 0;

            for (auto const& [ep, b] : m_buckets) {
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

            assert(computed_total == m_total_in_use && "m_total_in_use drift");
#else
#endif
        }

        /// @brief Determine wait reason for an endpoint under lock
        WaitReason determine_wait_reason_locked(
            Endpoint const& endpoint) const {
            auto buck_iterator = m_buckets.find(endpoint);
            if (buck_iterator == m_buckets.end()) {
                // New endpoint, check global capacity
                std::size_t total_total = m_total_in_use + total_idle_locked();
                return (total_total >= m_cfg.max_total_connections)
                           ? WaitReason::GlobalCapacity
                           : WaitReason::EndpointCapacity;
            }

            auto const& bucket = buck_iterator->second;
            std::size_t ep_total = bucket.in_use.size() + bucket.idle.size();

            if (ep_total >= m_cfg.max_connections_per_endpoint) {
                return WaitReason::EndpointCapacity;
            }
            return WaitReason::GlobalCapacity;
        }

        /// @brief Pop a waiter for the given endpoint under lock
        std::shared_ptr<boost::asio::steady_timer>
        pop_waiter_for_endpoint_locked(Endpoint const& endpoint) {
            // First: try endpoint-specific waiters (O(1))
            auto buck_iterator = m_buckets.find(endpoint);
            if (buck_iterator != m_buckets.end()) {
                auto& bucket = buck_iterator->second;
                while (!bucket.waiters.empty()) {
                    Waiter* waiter = bucket.waiters.front();
                    bucket.waiters.pop_front();  // Remove from secondary queue

                    if (waiter->active) {
                        waiter->active = false;
                        auto timer = waiter->timer;
                        m_metrics.waiters_total.fetch_sub(
                            1, std::memory_order_relaxed);
                        return timer;
                    }
                }
            }

            // Second: try "any endpoint" / global waiters (O(1))
            while (!m_global_waiters.empty()) {
                Waiter* global_waiter = m_global_waiters.front();
                m_global_waiters.pop_front();  // Remove from secondary queue

                if (global_waiter->active) {
                    global_waiter->active = false;
                    auto timer = global_waiter->timer;
                    m_metrics.waiters_total.fetch_sub(
                        1, std::memory_order_relaxed);
                    return timer;
                }
            }

            return {};
        }

        /// @brief Compute total number of idle connections under lock
        std::size_t total_idle_locked() const {
            std::size_t count = 0;
            for (auto const& [_endpoint, _bucket] : m_buckets) {
                count += _bucket.idle.size();
            }
            return count;
        }

        /// @brief Prune idle connections that have expired under lock
        void prune_idle_locked(std::chrono::steady_clock::time_point now) {
            if (m_cfg.connection_idle_ttl.count() <= 0) {
                return;
            }

            for (auto& [_endpoint, _bucket] : m_buckets) {
                while (!_bucket.idle.empty()) {
                    auto const& front = _bucket.idle.front();
                    if (now - front.last_used < m_cfg.connection_idle_ttl) {
                        break;
                    }

                    if (m_cfg.close_on_prune && front.conn) {
                        front.conn->close_http();
                        front.conn->close_https();
                    }
                    _bucket.idle.pop_front();
                    m_metrics.connection_pruned.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }

        /// @brief Report connection failure for circuit breaker
        /// @param ep Endpoint of the failed connection
        void report_connection_failure(Endpoint const& endpoint) {
            std::lock_guard<std::mutex> lock(mu_);
            auto& bucket = m_buckets[endpoint];

            bucket.consecutive_failures++;

            if (bucket.consecutive_failures >=
                m_cfg.circuit_breaker_failure_threshold) {
                bucket.circuit_open_until = std::chrono::steady_clock::now() +
                                            m_cfg.circuit_breaker_timeout;
                m_metrics.circuit_breaker_opened.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        /// @brief Report connection success for circuit breaker
        /// @param ep Endpoint of the successful connection
        void report_connection_success(Endpoint const& endpoint) {
            std::lock_guard<std::mutex> lock(mu_);
            auto bucket_iter = m_buckets.find(endpoint);
            if (bucket_iter == m_buckets.end()) {
                return;
            }

            auto& bucket = bucket_iter->second;
            if (bucket.consecutive_failures > 0) {
                bucket.consecutive_failures = 0;
                m_metrics.circuit_breaker_closed.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        /// @brief Basic health check for a connection
        bool is_connection_healthy(
            [[maybe_unused]] Conn const& conn) const noexcept {
            // Use the Connection's own health check if available.
            // This allows the pool to drop connections that are no longer
            // usable (e.g., closed sockets).
            return conn.is_healthy();
        }

        /// @brief Release a connection back to the pool
        void release(Endpoint const& endpoint, std::uint64_t ident) noexcept {
            std::shared_ptr<boost::asio::steady_timer> timer;

            try {
                std::lock_guard<std::mutex> lock(mu_);
                auto bucket_iter = m_buckets.find(endpoint);

                if (bucket_iter == m_buckets.end()) {
                    m_metrics.release_invalid_id.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                auto& bucket = bucket_iter->second;
                auto it2 = bucket.in_use.find(ident);

                if (it2 == bucket.in_use.end()) {
                    m_metrics.release_invalid_id.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }

                auto uptr_conn = std::move(it2->second);
                bucket.in_use.erase(it2);
                --m_total_in_use;
                m_metrics.total_in_use.store(m_total_in_use,
                                             std::memory_order_relaxed);

                // Health check before recycling
                bool healthy = uptr_conn && is_connection_healthy(*uptr_conn);

                if (healthy) {
                    auto now = std::chrono::steady_clock::now();
                    bucket.idle.push_back(IdleEntry{
                        .conn = std::move(uptr_conn),
                        .last_used = now,
                        .created = now,   // created time (will be set properly
                                          // on first create)
                        .reuse_count = 0  // reuse_count
                    });
                    m_metrics.total_idle.fetch_add(1,
                                                   std::memory_order_relaxed);
                } else {
                    m_metrics.connection_dropped_unhealthy.fetch_add(
                        1, std::memory_order_relaxed);
                    // Connection dropped, capacity freed
                }

                // Pop waiter UNDER LOCK, but don't cancel  just yet
                timer = pop_waiter_for_endpoint_locked(endpoint);

                check_invariants_locked();
            } catch (...) {
                // Swallow all exceptions in release to avoid throwing from
                // destructor
            }

            // Cancel OUTSIDE lock to avoid deadlock
            if (timer) {
                timer->cancel();
            }
        }

        /// @brief Try to acquire a connection under lock
        std::optional<Lease> try_acquire_locked(Endpoint const& endpoint) {
            // Check shutdown
            if (!m_state->alive.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            auto now = std::chrono::steady_clock::now();
            prune_idle_locked(now);

            auto& bucket = m_buckets[endpoint];

            // Check circuit breaker
            if (bucket.is_circuit_open(now)) {
                return std::nullopt;
            }

            // Prefer reusing idle connections
            while (!bucket.idle.empty()) {
                auto entry = std::move(bucket.idle.front());
                bucket.idle.pop_front();
                m_metrics.total_idle.fetch_sub(1, std::memory_order_relaxed);

                // Validate health
                if (!entry.conn || !is_connection_healthy(*entry.conn)) {
                    m_metrics.connection_dropped_unhealthy.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;  // Try next idle connection
                }

                // Check reuse limit
                if (entry.reuse_count >= m_cfg.max_connection_reuse_count) {
                    m_metrics.connection_dropped_reuse_limit.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }

                // Check age limit
                if (now - entry.created > m_cfg.max_connection_age) {
                    m_metrics.connection_dropped_age_limit.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }

                // Connection is good to reuse
                entry.reuse_count++;
                auto ident = m_next_id++;
                Conn* raw = entry.conn.get();
                bucket.in_use.emplace(ident, std::move(entry.conn));
                ++m_total_in_use;

                m_metrics.total_in_use.store(m_total_in_use,
                                             std::memory_order_relaxed);
                m_metrics.connection_reused.fetch_add(
                    1, std::memory_order_relaxed);

                check_invariants_locked();
                return Lease(
                    m_state, raw, endpoint, ident,
                    [this](Endpoint const& endpoint, std::uint64_t id_) {
                        release(endpoint, id_);
                    });
            }

            // No idle connections available, check capacity for creating new
            const std::size_t endpoint_total =
                bucket.in_use.size() + bucket.idle.size();
            const std::size_t total_total =
                m_total_in_use + total_idle_locked();

            if (endpoint_total >= m_cfg.max_connections_per_endpoint) {
                return std::nullopt;
            }
            if (total_total >= m_cfg.max_total_connections) {
                return std::nullopt;
            }

            // Create a new connection and mark it in-use
            auto connection_instance =
                std::make_unique<Conn>(m_ex, m_ssl_ctx, endpoint);
            Conn* raw = connection_instance.get();
            auto connection_id = m_next_id++;

            bucket.in_use.emplace(connection_id,
                                  std::move(connection_instance));
            ++m_total_in_use;

            m_metrics.total_in_use.store(m_total_in_use,
                                         std::memory_order_relaxed);
            m_metrics.connection_created.fetch_add(1,
                                                   std::memory_order_relaxed);

            check_invariants_locked();
            return Lease(m_state, raw, endpoint, connection_id,
                         [this](Endpoint const& endpoint, std::uint64_t id_) {
                             release(endpoint, id_);
                         });
        }

       private:
        boost::asio::any_io_executor m_ex;  ///< Executor for async operations
        boost::asio::ssl::context& m_ssl_ctx;    ///< SSL context for HTTPS
        AsyncConnectionPoolConfiguration m_cfg;  ///< Pool configuration

        mutable std::mutex mu_;  ///< Mutex for protecting internal state
        std::unordered_map<Endpoint, Bucket>
            m_buckets;  ///< Per-endpoint buckets

        std::list<Waiter> m_waiters;  ///< Stable iterators for removal
        std::list<Waiter*>
            m_global_waiters;  ///< Waiters waiting on global capacity

        std::size_t m_total_in_use{0};  ///< Total in-use connections
        std::uint64_t m_next_id{1};     ///< Next connection ID

        std::shared_ptr<typename Lease::State> m_state;  ///< Shared pool state
        ConnectionPoolMetrics m_metrics;  ///< Metrics for monitoring
    };

}  // namespace rest_cpp
