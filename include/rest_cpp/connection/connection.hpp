#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/system_error.hpp>
#include <variant>

#include "../endpoint.hpp"  // Endpoint, set_sni, init_tls_on_ssl_context
#include "../request.hpp"   // PreparedRequest
#include "../response.hpp"  // Response, parse_beast_response
#include "../result.hpp"    // Result, Error

namespace rest_cpp {

    /** @brief Communication mode for connections. */
    enum class Mode {
        Sync,  /**< Blocking operations. */
        Async  /**< Non-blocking operations using coroutines. */
    };

    /**
     * @brief Represents a single network connection to an endpoint.
     * @tparam mode The communication mode (Sync or Async).
     */
    template <Mode mode>
    class Connection {
       private:
        using tcp = boost::asio::ip::tcp;
        using HttpStream = boost::beast::tcp_stream;
        using HttpsStream = boost::beast::ssl_stream<boost::beast::tcp_stream>;
        using Stream = std::variant<std::monostate,  // "not connected yet"
                                    HttpStream, HttpsStream>;

        using ensure_ret_t = std::conditional_t<
            mode == Mode::Sync, boost::system::error_code,
            boost::asio::awaitable<boost::system::error_code>>;

        using request_ret_t =
            std::conditional_t<mode == Mode::Sync, Result<Response>,
                               boost::asio::awaitable<Result<Response>>>;

       public:
        /**
         * @brief Constructs a Connection.
         * @param executor The executor to use.
         * @param ssl_ctx The SSL context for HTTPS.
         * @param endpoint The target endpoint.
         */
        Connection(boost::asio::any_io_executor executor,
                   boost::asio::ssl::context& ssl_ctx, Endpoint endpoint)
            : m_ex(std::move(executor)),
              m_ssl_ctx(ssl_ctx),
              m_endpoint(std::move(endpoint)) {
            m_endpoint.normalize_default_port();
            m_endpoint.normalize_host();
        }

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) = delete;
        Connection& operator=(Connection&&) = delete;

        ~Connection() noexcept {
            close_http();
            close_https();
        }

        /// @brief Close HTTP connection if open (best-effort).
        void close_http() noexcept {
            boost::system::error_code ec;

            if (std::holds_alternative<Connection::HttpStream>(m_stream)) {
                auto& stream = std::get<Connection::HttpStream>(m_stream);

                auto shutdown_result =
                    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
                auto close_result = stream.socket().close(ec);

                (void)shutdown_result;
                (void)close_result;

                // mark connection as "no stream"
                m_stream.emplace<std::monostate>();
            }
        }

        /// @brief Close HTTPS connection if open (best-effort).
        /// @note No TLS shutdown is performed
        void close_https() noexcept {
            boost::system::error_code ec;

            if (std::holds_alternative<Connection::HttpsStream>(m_stream)) {
                auto& s = std::get<Connection::HttpsStream>(m_stream);

                // No TLS shutdown Just close the underlying
                // TCP socket.
                boost::beast::get_lowest_layer(s).socket().shutdown(
                    tcp::socket::shutdown_both, ec);
                boost::beast::get_lowest_layer(s).socket().close(ec);

                // Clear the variant
                m_stream.emplace<std::monostate>();
            }
        }

        /**
         * @brief Ensures the connection is open, performing DNS and handshake if needed.
         * @param resolver The DNS resolver.
         * @param ec Error code output (Sync mode only).
         * @return In Sync mode: error code. In Async mode: awaitable error code.
         */
        ensure_ret_t ensure_connected(tcp::resolver& resolver,
                                      boost::system::error_code& ec) {
            if constexpr (mode == Mode::Sync) {
                ensure_connected_sync(resolver, ec);
                return ec;
            } else {
                (void)resolver;
                (void)ec;
                return ensure_connected_async();
            }
        }

        request_ret_t request(const PreparedRequest& preq,
                              tcp::resolver& resolver,
                              boost::system::error_code& ec) {
            if constexpr (mode == Mode::Sync) {
                return request_sync(preq, resolver, ec);
            } else {
                (void)resolver;
                (void)ec;
                return request_async(preq);
            }
        }

        /**
         * @brief Returns the endpoint this connection is tied to.
         */
        Endpoint endpoint() const { return m_endpoint; }

        /**
         * @brief Checks if the connection is currently open and healthy.
         */
        bool is_healthy() const noexcept {
            return std::visit(
                [](auto const& s) -> bool {
                    using T = std::decay_t<decltype(s)>;

                    if constexpr (std::is_same_v<T, std::monostate>) {
                        return false;
                    } else if constexpr (std::is_same_v<
                                             T, Connection::HttpStream>) {
                        return s.socket().is_open();
                    } else {  // Connection::HttpsStream
                        return boost::beast::get_lowest_layer(s)
                            .socket()
                            .is_open();
                    }
                },
                m_stream);
        }

       private:
        // -------- one HTTP transaction (sync/async) --------

        Result<Response> request_sync(const PreparedRequest& preq,
                                      tcp::resolver& resolver,
                                      boost::system::error_code& ec) {
            if (preq.ep != m_endpoint) {
                Error e{};
                e.code = Error::Code::InvalidUrl;
                e.message =
                    "PreparedRequest endpoint does not match Connection "
                    "endpoint";
                return Result<Response>::err(std::move(e));
            }

            ensure_connected_sync(resolver, ec);
            if (ec) return err_from_ec(ec);

            namespace http = boost::beast::http;
            http::response<http::string_body> beast_res;

            if (!m_endpoint.https) {
                auto& stream = std::get<HttpStream>(m_stream);

                http::write(stream, preq.beast_req, ec);
                if (ec) {
                    close_http();
                    return err_from_ec(ec);
                }

                m_buffer.clear();
                http::read(stream, m_buffer, beast_res, ec);
                if (ec) {
                    close_http();
                    return err_from_ec(ec);
                }
            } else {
                auto& stream = std::get<HttpsStream>(m_stream);

                http::write(stream, preq.beast_req, ec);
                if (ec) {
                    close_https();
                    return err_from_ec(ec);
                }

                m_buffer.clear();
                http::read(stream, m_buffer, beast_res, ec);
                if (ec) {
                    close_https();
                    return err_from_ec(ec);
                }
            }

            if (!beast_res.keep_alive()) {
                if (m_endpoint.https)
                    close_https();
                else
                    close_http();
            }

            return Result<Response>::ok(
                parse_beast_response(std::move(beast_res)));
        }

        boost::asio::awaitable<Result<Response>> request_async(
            const PreparedRequest& preq) {
            boost::system::error_code ec;

            if (preq.ep != m_endpoint) {
                Error e{};
                e.code = Error::Code::InvalidUrl;
                e.message =
                    "PreparedRequest endpoint does not match Connection "
                    "endpoint";
                co_return Result<Response>::err(std::move(e));
            }

            ec = co_await ensure_connected_async();
            if (ec) co_return err_from_ec(ec);

            namespace http = boost::beast::http;
            http::response<http::string_body> beast_res;

            if (!m_endpoint.https) {
                auto& stream = std::get<HttpStream>(m_stream);

                co_await http::async_write(stream, preq.beast_req,
                                           boost::asio::redirect_error(
                                               boost::asio::use_awaitable, ec));
                if (ec) {
                    close_http();
                    co_return err_from_ec(ec);
                }

                m_buffer.clear();
                co_await http::async_read(stream, m_buffer, beast_res,
                                          boost::asio::redirect_error(
                                              boost::asio::use_awaitable, ec));
                if (ec) {
                    close_http();
                    co_return err_from_ec(ec);
                }
            } else {
                auto& stream = std::get<HttpsStream>(m_stream);

                co_await http::async_write(stream, preq.beast_req,
                                           boost::asio::redirect_error(
                                               boost::asio::use_awaitable, ec));
                if (ec) {
                    close_https();
                    co_return err_from_ec(ec);
                }

                m_buffer.clear();
                co_await http::async_read(stream, m_buffer, beast_res,
                                          boost::asio::redirect_error(
                                              boost::asio::use_awaitable, ec));
                if (ec) {
                    close_https();
                    co_return err_from_ec(ec);
                }
            }

            if (!beast_res.keep_alive()) {
                if (m_endpoint.https)
                    close_https();
                else
                    close_http();
            }

            co_return Result<Response>::ok(
                parse_beast_response(std::move(beast_res)));
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_connected_async() {
            if (m_endpoint.https)
                co_return co_await ensure_https_connected_async();
            co_return co_await ensure_http_connected_async();
        }

        void ensure_connected_sync(tcp::resolver& resolver,
                                   boost::system::error_code& ec) {
            if (m_endpoint.https) {
                ensure_https_connected_sync(resolver, ec);
            } else {
                ensure_http_connected_sync(resolver, ec);
            }
        }

        static Result<Response> err_from_ec(
            const boost::system::error_code& ec) {
            Error e{};
            e.code = Error::Code::NetworkError;  // adjust to your enum
            e.message = ec.message();
            return Result<Response>::err(std::move(e));
        }

        bool ensure_http_connected_sync(tcp::resolver& resolver,
                                        boost::system::error_code& ec) {
            // if variant holds HTTPS, drop it
            if (std::holds_alternative<HttpsStream>(m_stream)) close_https();

            // reuse if already HTTP + open
            if (std::holds_alternative<HttpStream>(m_stream)) {
                auto& s = std::get<HttpStream>(m_stream);
                if (s.socket().is_open()) {
                    ec.clear();
                    return true;
                }
            }

            close_http();

            auto results =
                resolver.resolve(m_endpoint.host, m_endpoint.port, ec);
            if (ec) return false;

            m_stream.emplace<HttpStream>(resolver.get_executor());
            auto& s = std::get<HttpStream>(m_stream);
            s.connect(results, ec);
            return !ec;
        }

        bool ensure_https_connected_sync(tcp::resolver& resolver,
                                         boost::system::error_code& ec) {
            if (std::holds_alternative<HttpStream>(m_stream)) close_http();

            if (std::holds_alternative<HttpsStream>(m_stream)) {
                auto& s = std::get<HttpsStream>(m_stream);
                if (boost::beast::get_lowest_layer(s).socket().is_open()) {
                    ec.clear();
                    return true;
                }
            }

            close_https();

            auto results =
                resolver.resolve(m_endpoint.host, m_endpoint.port, ec);
            if (ec) return false;

            m_stream.emplace<HttpsStream>(resolver.get_executor(), m_ssl_ctx);
            auto& s = std::get<HttpsStream>(m_stream);

            boost::beast::get_lowest_layer(s).connect(results, ec);
            if (ec) return false;

            if (!set_sni(s, m_endpoint.host, ec)) return false;

            s.handshake(boost::asio::ssl::stream_base::client, ec);
            if (ec) {
                close_https();
                return false;
            }

            return true;
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_http_connected_async() {
            boost::system::error_code ec;

            if (std::holds_alternative<HttpsStream>(m_stream)) close_https();

            if (std::holds_alternative<HttpStream>(m_stream)) {
                auto& s = std::get<HttpStream>(m_stream);
                if (s.socket().is_open()) co_return ec;
            }

            close_http();

            tcp::resolver resolver(m_ex);
            auto results = co_await resolver.async_resolve(
                m_endpoint.host, m_endpoint.port,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) co_return ec;

            m_stream.emplace<HttpStream>(m_ex);
            auto& s = std::get<HttpStream>(m_stream);

            co_await s.async_connect(
                results,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec) close_http();
            co_return ec;
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_https_connected_async() {
            boost::system::error_code ec;

            if (std::holds_alternative<HttpStream>(m_stream)) close_http();

            if (std::holds_alternative<HttpsStream>(m_stream)) {
                auto& s = std::get<HttpsStream>(m_stream);
                if (boost::beast::get_lowest_layer(s).socket().is_open())
                    co_return ec;
            }

            close_https();

            tcp::resolver resolver(m_ex);
            auto results = co_await resolver.async_resolve(
                m_endpoint.host, m_endpoint.port,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) co_return ec;

            m_stream.emplace<HttpsStream>(m_ex, m_ssl_ctx);
            auto& s = std::get<HttpsStream>(m_stream);

            co_await boost::beast::get_lowest_layer(s).async_connect(
                results,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) {
                close_https();
                co_return ec;
            }

            if (!set_sni(s, m_endpoint.host, ec)) {
                close_https();
                co_return ec;
            }

            co_await s.async_handshake(
                boost::asio::ssl::stream_base::client,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec) close_https();
            co_return ec;
        }

        boost::asio::any_io_executor m_ex;
        boost::asio::ssl::context& m_ssl_ctx;

        Endpoint m_endpoint{};
        boost::beast::flat_buffer m_buffer{};

        Stream m_stream;
    };

}  // namespace rest_cpp
