#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "response.hpp"
#include "rest_cpp/url.hpp"
#include "result.hpp"

namespace rest_cpp {
    using tcp = boost::asio::ip::tcp;

    class RestClient {
       public:
        explicit RestClient(RestClientConfiguration config);
        ~RestClient() noexcept;

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
        // Members
        RestClientConfiguration m_config{};
        boost::asio::io_context io_{1};
        tcp::resolver m_resolver{io_};
        std::string m_port;
        std::string m_host;
        std::optional<boost::beast::tcp_stream> m_http_stream;
        std::optional<boost::beast::ssl_stream<boost::beast::tcp_stream>>
            m_https_stream;
        boost::asio::ssl::context ssl_ctx_{
            boost::asio::ssl::context::tls_client};

        // Internal Helpers
        // Http helpers
        void close_http() noexcept;

        bool ensure_http_connected(const ParsedUrl& u,
                                   boost::system::error_code& ec);
        // Https helpers
        void close_https() noexcept;
        bool ensure_https_connected(const ParsedUrl& u,
                                    boost::system::error_code& ec);
        bool m_active_https_connection{false};

        void init_tls();
    };

}  // namespace rest_cpp
