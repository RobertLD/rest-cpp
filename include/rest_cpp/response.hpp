#pragma once

#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <map>
#include <string>

namespace rest_cpp {

    struct Response {
        int status_code{0};
        std::map<std::string, std::string> headers;
        std::string body;
    };

    /// @brief Copy Boost.Beast response headers into a std::map.
    /// @note If duplicate header keys occur, the last one wins.
    inline void copy_response_headers(const boost::beast::http::fields& in,
                                      std::map<std::string, std::string>& out) {
        out.clear();
        for (auto const& field : in) {
            out[std::string(field.name_string())] = std::string(field.value());
        }
    }

    /// @brief Convert a Boost.Beast HTTP response to a rest_cpp::Response.
    /// @param beast_res The Boost.Beast HTTP response.
    /// @return A rest_cpp::Response with status, headers, and body.
    inline Response parse_beast_response(
        const boost::beast::http::response<boost::beast::http::string_body>&
            beast_res) {
        Response out;
        out.status_code = static_cast<int>(beast_res.result_int());
        for (const auto& field : beast_res.base()) {
            out.headers[field.name_string().to_string()] =
                field.value().to_string();
        }
        out.body = beast_res.body();
        return out;
    }

}  // namespace rest_cpp