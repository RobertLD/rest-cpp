#pragma once

#include <memory>
#include <string>
#include <vector>

#include "request.hpp"
#include "url.hpp"

namespace rest_cpp {

    /// @brief Interface for intercepting and modifying requests before they are
    /// sent.
    class RequestInterceptor {
       public:
        virtual ~RequestInterceptor() = default;

        /// @brief Perform modifications on the outgoing request.
        /// @param req The request to modify.
        /// @param url The resolved URL components for the request.
        virtual void prepare(Request& req, const UrlComponents& url) const = 0;
    };

    /// @brief Interceptor for Bearer Token authentication.
    class BearerAuthInterceptor : public RequestInterceptor {
       public:
        explicit BearerAuthInterceptor(std::string token)
            : token_(std::move(token)) {}

        void prepare(Request& req,
                     const UrlComponents& /*url*/) const override {
            req.headers["Authorization"] = "Bearer " + token_;
        }

       private:
        std::string token_;
    };

    /// @brief Interceptor for API Key authentication.
    class ApiKeyInterceptor : public RequestInterceptor {
       public:
        enum class Location : std::uint8_t { Header, Query };

        explicit ApiKeyInterceptor(std::string key, std::string value,
                                   Location loc = Location::Header)
            : key_(std::move(key)), value_(std::move(value)), loc_(loc) {}

        void prepare(Request& req,
                     const UrlComponents& /*url*/) const override {
            if (loc_ == Location::Header) {
                req.headers[key_] = value_;
            } else {
                // Find fragment start
                size_t fragment_pos = req.url.find('#');
                std::string fragment;
                if (fragment_pos != std::string::npos) {
                    fragment = req.url.substr(fragment_pos);
                    req.url.erase(fragment_pos);
                }

                // Append query param
                if (req.url.find('?') == std::string::npos) {
                    req.url += "?";
                } else {
                    char last = req.url.back();
                    if (last != '?' && last != '&') {
                        req.url += "&";
                    }
                }
                req.url += url_utils::url_encode(key_) + "=" + url_utils::url_encode(value_);
                req.url += fragment;
            }
        }

       private:
        std::string key_;
        std::string value_;
        Location loc_;
    };

}  // namespace rest_cpp
