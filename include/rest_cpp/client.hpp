#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "response.hpp"
#include "result.hpp"

namespace rest_cpp {

    class RestClient {
       public:
        explicit RestClient(RestClientConfiguration config);
        ~RestClient();

        RestClient(const RestClient&) = delete;
        RestClient& operator=(const RestClient&) = delete;

        RestClient(RestClient&&) = delete;
        RestClient& operator=(RestClient&&) = delete;

        [[nodiscard]] const RestClientConfiguration& config() const noexcept;

        [[nodiscard]] Result<Response> send(const Request& request);

        // Convenience verbs
        [[nodiscard]] Result<Response> get(const std::string& url);
        [[nodiscard]] Result<Response> head(const std::string& url);
        [[nodiscard]] Result<Response> del(const std::string& url);
        [[nodiscard]] Result<Response> options(const std::string& url);

        [[nodiscard]] Result<Response> post(const std::string& url,
                                            std::string body);
        [[nodiscard]] Result<Response> put(const std::string& url,
                                           std::string body);
        [[nodiscard]] Result<Response> patch(const std::string& url,
                                             std::string body);

       private:
        RestClientConfiguration m_config{};
        boost::asio::io_context io_{1};
        boost::asio::ssl::context ssl_ctx_{
            boost::asio::ssl::context::tls_client};

        void init_tls();
    };

}  // namespace rest_cpp
