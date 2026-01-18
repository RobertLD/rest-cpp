#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "response.hpp"
#include "rest_cpp/connection.hpp"
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
        std::optional<UrlComponents> m_base_url;

        boost::asio::io_context io_{1};
        tcp::resolver m_resolver{io_};

        boost::asio::ssl::context m_ssl_context{
            boost::asio::ssl::context::tls_client};

        Connection<Mode::Sync> m_conn{io_.get_executor(), m_ssl_context};

        // Helpers
        [[nodiscard]] inline Result<UrlComponents> resolve_request_url(
            std::string_view url) const {
            const rest_cpp::UrlComponents* base =
                m_base_url ? &*m_base_url : nullptr;
            return rest_cpp::url_utils::resolve_url(url, base);
        }
    };

}  // namespace rest_cpp
