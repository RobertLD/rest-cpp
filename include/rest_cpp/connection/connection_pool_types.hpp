#pragma once

#include <atomic>
#include <cstdint>

namespace rest_cpp {

    /// @brief Error codes for connection acquisition failures

    enum class AcquireError {
        Timeout,   ///< Timeout expired, resource may become available later
        Shutdown,  ///< Pool is shutting down, will never succeed
        InternalError,  ///< Unexpected error (e.g., timer failure)
        CircuitOpen     ///< Circuit breaker open for this endpoint
    };

    /// @brief Convert AcquireError to string for logging or diagnostics
    inline const char* to_string(AcquireError err) {
        switch (err) {
            case AcquireError::Timeout:
                return "Timeout";
            case AcquireError::Shutdown:
                return "Shutdown";
            case AcquireError::InternalError:
                return "InternalError";
            case AcquireError::CircuitOpen:
                return "CircuitOpen";
        }
        return "Unknown";
    }

    /// @brief Metrics for monitoring connection pool behavior
    struct ConnectionPoolMetrics {
        // Gauges (current state)
        std::atomic<std::size_t> total_in_use{0};  ///< Currently leased out
        std::atomic<std::size_t> total_idle{0};    ///< Currently idle
        std::atomic<std::size_t> waiters_total{
            0};  ///< Currently waiting for a connection

        // Counters (cumulative)
        std::atomic<std::uint64_t> acquire_success{0};  ///< Successful acquires
        std::atomic<std::uint64_t> acquire_timeout{0};  ///< Acquire timed out
        std::atomic<std::uint64_t> acquire_shutdown{0};  ///< Pool shutdown
        std::atomic<std::uint64_t> acquire_internal_error{
            0};                                              ///< Internal error
        std::atomic<std::uint64_t> acquire_circuit_open{0};  ///< Circuit open
        std::atomic<std::uint64_t> connection_created{0};  ///< New connections
        std::atomic<std::uint64_t> connection_reused{0};   ///< Reused idle
        std::atomic<std::uint64_t> connection_pruned{0};   ///< Pruned idle
        std::atomic<std::uint64_t> connection_dropped_unhealthy{
            0};  ///< Dropped unhealthy
        std::atomic<std::uint64_t> connection_dropped_reuse_limit{
            0};  ///< Dropped due to reuse limit
        std::atomic<std::uint64_t> connection_dropped_age_limit{
            0};  ///< Dropped due to age limit

        std::atomic<std::uint64_t> release_invalid_id{
            0};  ///< Released unknown connection
        std::atomic<std::uint64_t> circuit_breaker_opened{
            0};  ///< Circuit breaker opened
        std::atomic<std::uint64_t> circuit_breaker_closed{
            0};  ///< Circuit breaker closed
    };

}  // namespace rest_cpp
