#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "rest_cpp/error.hpp"
#include "rest_cpp/result.hpp"
namespace http = boost::beast::http;

namespace rest_cpp {
    enum class HttpMethod {
        Get,
        Post,
        Put,
        Patch,
        Delete,
        Head,
        Options,
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