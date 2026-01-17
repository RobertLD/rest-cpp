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

        /// Verify SSL certificates for HTTPS requests.
        bool verify_tls{true};
    };
}  // namespace rest_cpp