#pragma once
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace rest_cpp {
    struct RestClientConfiguration {
        /// Optional base URL for the client. If set, all requests will be
        /// relative to this URL.
        std::optional<std::string> base_url;

        /// User-Agent string sent with each request.
        std::string user_agent{"rest_cpp_client/1.0"};

        /// Default headers to include in every request.
        std::map<std::string, std::string> default_headers;

        /// Timeout for establishing a connection, in milliseconds.
        std::chrono::milliseconds connect_timeout{5000};

        /// Timeout for receiving a response to a request, in milliseconds.
        std::chrono::milliseconds request_timeout{5000};

        // Maximum size of response bodies, in bytes.
        size_t max_body_bytes{10 * 1024 * 1024};  // 10 MB

        /// Verify SSL certificates for HTTPS requests.
        bool verify_tls{true};
    };

    struct AsyncConnectionPoolConfiguration {
        /// Maximum number of connections in the connection pool.
        size_t max_total_connections{10};
        size_t max_connections_per_endpoint{5};
        std::chrono::milliseconds connection_idle_ttl{30000};  // 30s

        /// Close connections when pruning idle connections
        bool close_on_prune{true};

        /// Close connections on pool shutdown
        bool close_on_shutdown{true};

        /// Maximum times a connection can be reused before being discarded
        size_t max_connection_reuse_count{1000};

        /// Maximum age of a connection before being discarded
        std::chrono::seconds max_connection_age{300};  // 5 minutes

        /// Number of consecutive failures before opening circuit breaker
        size_t circuit_breaker_failure_threshold{5};

        /// Time to wait before retrying after circuit breaker opens
        std::chrono::seconds circuit_breaker_timeout{30};
    };

    struct AsyncRestClientConfiguration : public RestClientConfiguration {
        /// Maximum number of connections in the connection pool.
        AsyncConnectionPoolConfiguration pool_config;
    };
}  // namespace rest_cpp