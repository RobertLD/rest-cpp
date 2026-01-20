#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "response.hpp"
#include "rest_cpp/connection/connection.hpp"
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
        RestClientConfiguration m_config{};
        std::optional<UrlComponents> m_base_url;

        boost::asio::io_context io_{1};
        tcp::resolver m_resolver{io_};

        boost::asio::ssl::context m_ssl_context{
            boost::asio::ssl::context::tls_client};

        std::optional<Connection<Mode::Sync>> m_conn;
        std::optional<Endpoint> m_conn_endpoint;

        void ensure_connection_for(const Endpoint& ep);

        static Endpoint endpoint_from_url(const UrlComponents& u) {
            Endpoint ep;
            ep.host = u.host;
            ep.port = u.port;
            ep.https = u.https;
            return ep;
        }

        [[nodiscard]] inline Result<UrlComponents> resolve_request_url(
            std::string_view url) const {
            const rest_cpp::UrlComponents* base =
                m_base_url ? &*m_base_url : nullptr;
            return rest_cpp::url_utils::resolve_url(url, base);
        }
    };

}  // namespace rest_cpp
