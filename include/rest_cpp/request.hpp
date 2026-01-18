#pragma once
#include <optional>
#include <string>
#include <unordered_map>

#include "http_method.hpp"
#include "url.hpp"

namespace rest_cpp {

    struct Request {
        HttpMethod method;
        std::string url;
        std::unordered_map<std::string, std::string> headers;
        std::optional<std::string> body;
    };

    struct PreparedRequest {
        UrlComponents url;
        boost::beast::http::request<boost::beast::http::string_body> beast_req;
    };

    /// @brief Apply Request headers into a Boost.Beast header container.
    /// @note Uses `set()`, so duplicate keys overwrite previous values.
    inline void apply_request_headers(
        const std::unordered_map<std::string, std::string>& in,
        boost::beast::http::fields& out) {
        for (const auto& [k, v] : in) {
            out.set(k, v);
        }
    }

    inline boost::beast::http::request<boost::beast::http::string_body>
    prepare_beast_request(const Request& req, const UrlComponents& url,
                          const std::string& user_agent,
                          const bool keep_alive = true) {
        namespace http = boost::beast::http;
        http::request<http::string_body> beast_req;
        beast_req.version(11);
        beast_req.method(to_boost_http_method(req.method));
        beast_req.target(url.target);
        beast_req.set(http::field::host, url.host);
        beast_req.set(http::field::user_agent, user_agent);
        beast_req.keep_alive(keep_alive);
        apply_request_headers(req.headers, beast_req.base());
        if (req.body.has_value()) {
            beast_req.body() = *req.body;
            beast_req.prepare_payload();
        }
        return beast_req;
    }

}  // namespace rest_cpp