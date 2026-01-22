#pragma once
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace rest_cpp {
    /**
     * @brief Configuration for the synchronous RestClient.
     */
    struct RestClientConfiguration {
        /** @brief Optional base URL for the client. */
        std::optional<std::string> base_url;

        /** @brief User-Agent string sent with each request. */
        std::string user_agent{"rest_cpp_client/1.0"};

        /** @brief Default headers to include in every request. */
        std::map<std::string, std::string> default_headers;

        /** @brief Timeout for establishing a connection. */
        std::chrono::milliseconds connect_timeout{5000};

        /** @brief Timeout for receiving a response. */
        std::chrono::milliseconds request_timeout{5000};

        /** @brief Maximum size of response bodies in bytes. */
        size_t max_body_bytes{static_cast<size_t>(10) * 1024U * 1024U};

        /** @brief Whether to verify SSL certificates. */
        bool verify_tls{true};

        /** @brief Middleware interceptors for request manipulation. */
        std::vector<std::shared_ptr<const class RequestInterceptor>> interceptors;
    };

    /**
     * @brief Configuration for the asynchronous connection pool.
     */
    struct AsyncConnectionPoolConfiguration {
        /** @brief Maximum total connections in the pool. */
        size_t max_total_connections{10};
        /** @brief Maximum connections per endpoint (host:port). */
        size_t max_connections_per_endpoint{5};
        /** @brief Idle connection time-to-live. */
        std::chrono::milliseconds connection_idle_ttl{30000};

        /** @brief Whether to close connections when pruning idle ones. */
        bool close_on_prune{true};

        /** @brief Whether to close connections on pool shutdown. */
        bool close_on_shutdown{true};

        /** @brief Max requests per connection before forcing rotation. */
        size_t max_connection_reuse_count{1000};

        /** @brief Max lifetime of any single connection. */
        std::chrono::seconds max_connection_age{300};

        /** @brief Consecutive failures triggering circuit breaker. */
        size_t circuit_breaker_failure_threshold{5};

        /** @brief Duration the circuit breaker remains open after tripping. */
        std::chrono::seconds circuit_breaker_timeout{30};
    };

    struct AsyncRestClientConfiguration : public RestClientConfiguration {
        /// Maximum number of connections in the connection pool.
        AsyncConnectionPoolConfiguration pool_config;
    };
}  // namespace rest_cpp