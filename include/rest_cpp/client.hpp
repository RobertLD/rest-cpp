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
#include "serialize_impl.hpp"

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

        // Templated versions for automatic serialization

        template <typename T>
        [[nodiscard]] Result<T> get(const std::string& url) {
            return to_result_t<T>(get(url));
        }

        template <typename T>
        [[nodiscard]] Result<T> head(const std::string& url) {
            return to_result_t<T>(head(url));
        }

        template <typename T>
        [[nodiscard]] Result<T> del(const std::string& url) {
            return to_result_t<T>(del(url));
        }

        template <typename T>
        [[nodiscard]] Result<T> options(const std::string& url) {
            return to_result_t<T>(options(url));
        }

        template <typename T>
        [[nodiscard]] Result<T> post(const std::string& url, std::string body) {
            return to_result_t<T>(post(url, std::move(body)));
        }

        template <typename T>
        [[nodiscard]] Result<T> put(const std::string& url, std::string body) {
            return to_result_t<T>(put(url, std::move(body)));
        }

        template <typename T>
        [[nodiscard]] Result<T> patch(const std::string& url,
                                      std::string body) {
            return to_result_t<T>(patch(url, std::move(body)));
        }

       private:
        template <typename T>
        Result<T> to_result_t(Result<Response>&& res) {
            if (res.has_error()) {
                return Result<T>::err(res.error());
            }
            T out;
            // Lookup deserialize via ADL or from included headers
            deserialize(res.value(), out);
            return Result<T>::ok(std::move(out));
        }

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
