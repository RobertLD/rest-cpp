#pragma once

#include <boost/beast/http/fields.hpp>
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

}  // namespace rest_cpp