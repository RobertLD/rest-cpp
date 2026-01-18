#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "rest_cpp/connection.hpp"
#include "rest_cpp/url.hpp"

namespace rest_cpp {

    class AsyncRestClient {
       public:
        using tcp = boost::asio::ip::tcp;
        using awaitable_result = boost::asio::awaitable<Result<Response>>;

        explicit AsyncRestClient(RestClientConfiguration config);
        ~AsyncRestClient() noexcept;

        AsyncRestClient(const AsyncRestClient&) = delete;
        AsyncRestClient& operator=(const AsyncRestClient&) = delete;
        AsyncRestClient(AsyncRestClient&&) = delete;
        AsyncRestClient& operator=(AsyncRestClient&&) = delete;

        [[nodiscard]] const RestClientConfiguration& config() const noexcept;

        // Let callers drive the event loop
        [[nodiscard]] boost::asio::io_context& context() noexcept {
            return io_;
        }

        // Coroutine API: "looks like sync", but you co_await it.
        [[nodiscard]] awaitable_result send(const Request& request);

        // Convenience verbs (same names/signature style as sync, but awaitable)
        [[nodiscard]] awaitable_result get(const std::string& url);
        [[nodiscard]] awaitable_result head(const std::string& url);
        [[nodiscard]] awaitable_result del(const std::string& url);
        [[nodiscard]] awaitable_result options(const std::string& url);

        [[nodiscard]] awaitable_result post(const std::string& url,
                                            std::string body);
        [[nodiscard]] awaitable_result put(const std::string& url,
                                           std::string body);
        [[nodiscard]] awaitable_result patch(const std::string& url,
                                             std::string body);

       private:
        // ---- state / config ----
        RestClientConfiguration m_config{};
        std::optional<UrlComponents> m_base_url;

        boost::asio::io_context io_{1};
        tcp::resolver m_resolver{io_};

        // NOTE: with a single connection + single buffer, this client supports
        // one in-flight request at a time unless you add pooling or per-op
        // buffer/state.
        boost::beast::flat_buffer m_buffer{};

        ConnectionDetails m_connection_details{};

        std::optional<boost::beast::tcp_stream> m_http_stream;
        std::optional<boost::beast::ssl_stream<boost::beast::tcp_stream>>
            m_https_stream;

        boost::asio::ssl::context m_ssl_context{
            boost::asio::ssl::context::tls_client};

        // ---- teardown helpers ----
        void close_http() noexcept;
        void close_https() noexcept;

        // ---- connection helpers (coroutine) ----
        [[nodiscard]] boost::asio::awaitable<boost::system::error_code>
        ensure_http_connected(const UrlComponents& u);

        [[nodiscard]] boost::asio::awaitable<boost::system::error_code>
        ensure_https_connected(const UrlComponents& u);
    };

}  // namespace rest_cpp
