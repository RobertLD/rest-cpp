#pragma once

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <string>
#include <unordered_map>

namespace rest_cpp {

    /**
     * @brief Represents an HTTP response.
     */
    struct Response {
        /** @brief HTTP status code (e.g., 200, 404). */
        int status_code{0};
        /** @brief HTTP response headers. */
        std::unordered_map<std::string, std::string> headers;
        /** @brief HTTP response body as a string. */
        std::string body;
    };

    /// @brief Copy Boost.Beast response headers into a std::unordered_map.
    /// @note If duplicate header keys occur, the last one wins.
    inline void copy_response_headers(
        const boost::beast::http::fields& in,
        std::unordered_map<std::string, std::string>& out) {
        out.clear();
        for (auto const& field : in) {
            out[std::string(field.name_string())] = std::string(field.value());
        }
    }

    /// @brief Convert a Boost.Beast HTTP response to a rest_cpp::Response.
    /// @param beast_res The Boost.Beast HTTP response.
    /// @return A rest_cpp::Response with status, headers, and body.
    inline Response parse_beast_response(
        boost::beast::http::response<boost::beast::http::string_body>&&
            beast_res) {
        Response out;
        out.status_code = static_cast<int>(beast_res.result_int());

        for (const auto& field : beast_res.base()) {
            // field.name_string() and field.value() are string_view-like
            out.headers.emplace(std::string(field.name_string()),
                                std::string(field.value()));
        }

        // Move body instead of copying
        out.body = std::move(beast_res.body());
        return out;
    }

}  // namespace rest_cpp