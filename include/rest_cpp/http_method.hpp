#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace http = boost::beast::http;

namespace rest_cpp {
    /**
     * @brief Supported HTTP methods.
     */
    enum class HttpMethod {
        Get,      /**< GET method */
        Post,     /**< POST method */
        Put,      /**< PUT method */
        Patch,    /**< PATCH method */
        Delete,   /**< DELETE method */
        Head,     /**< HEAD method */
        Options,  /**< OPTIONS method */
    };

    inline constexpr http::verb to_boost_http_method(HttpMethod method) {
        switch (method) {
            case HttpMethod::Get:
                return http::verb::get;
            case HttpMethod::Post:
                return http::verb::post;
            case HttpMethod::Put:
                return http::verb::put;
            case HttpMethod::Patch:
                return http::verb::patch;
            case HttpMethod::Delete:
                return http::verb::delete_;
            case HttpMethod::Head:
                return http::verb::head;
            case HttpMethod::Options:
                return http::verb::options;
            default:
                return http::verb::unknown;
        }
    }

}  // namespace rest_cpp