#pragma once
#include <map>
#include <optional>
#include <string>

#include "http_method.hpp"

namespace rest_cpp {

    struct Request {
        HttpMethod method;
        std::string url;
        std::map<std::string, std::string> headers;
        std::optional<std::string> body;
    };

    /// @brief Apply Request headers into a Boost.Beast header container.
    /// @note Uses `set()`, so duplicate keys overwrite previous values.
    inline void apply_request_headers(
        const std::map<std::string, std::string>& in,
        boost::beast::http::fields& out) {
        for (const auto& [k, v] : in) {
            out.set(k, v);
        }
    }

}  // namespace rest_cpp