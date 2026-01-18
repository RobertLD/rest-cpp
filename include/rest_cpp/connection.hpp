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
#include <optional>
#include <string>
#include <type_traits>

#include "endpoint.hpp"  // Endpoint, set_sni, init_tls_on_ssl_context
#include "request.hpp"   // PreparedRequest
#include "response.hpp"  // Response, parse_beast_response
#include "result.hpp"    // Result, Error
#include "url.hpp"       // UrlComponents

namespace rest_cpp {

    enum class Mode { Sync, Async };

    template <Mode mode>
    class Connection {
       private:
        using tcp = boost::asio::ip::tcp;

        using ensure_ret_t = std::conditional_t<
            mode == Mode::Sync, boost::system::error_code,
            boost::asio::awaitable<boost::system::error_code>>;

        using request_ret_t =
            std::conditional_t<mode == Mode::Sync, Result<Response>,
                               boost::asio::awaitable<Result<Response>>>;

       public:
        Connection(boost::asio::any_io_executor ex,
                   boost::asio::ssl::context& ssl_ctx)
            : m_ex(std::move(ex)), m_ssl_ctx(ssl_ctx) {}

        ~Connection() noexcept {
            close_http();
            close_https();
        }

        /// @brief Close HTTP connection if open (best-effort).
        void close_http() noexcept {
            boost::system::error_code ec;
            if (m_http_stream) {
                auto shutdown_result = m_http_stream->socket().shutdown(
                    tcp::socket::shutdown_both, ec);
                (void)shutdown_result;  // suppress unused warning
                auto close_result = m_http_stream->socket().close(ec);
                (void)close_result;  // suppress bitching hoe
                m_http_stream.reset();
            }
        }

        /// @brief Close HTTPS connection if open (best-effort).
        /// @note Keep this non-blocking: do NOT attempt TLS shutdown here.
        void close_https() noexcept {
            boost::system::error_code ec;
            if (m_https_stream) {
                // Shutdown stream
                auto shutdown_result = m_https_stream->shutdown(ec);
                (void)shutdown_result;

                auto close_result =
                    boost::beast::get_lowest_layer(*m_https_stream)
                        .socket()
                        .close(ec);
                (void)close_result;  // suppress bitching hoe
                m_https_stream.reset();
            }
        }

        ensure_ret_t ensure_connected(const UrlComponents& u,
                                      tcp::resolver& resolver,
                                      boost::system::error_code& ec) {
            if constexpr (mode == Mode::Sync) {
                ensure_connected_sync(u, resolver, ec);
                return ec;  // ok: returns error_code
            } else {
                (void)resolver;
                (void)ec;
                return ensure_connected_async(
                    u);  // return awaitable<error_code>
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

        Endpoint endpoint() const { return m_endpoint; }

       private:
        // -------- one HTTP transaction (sync/async) --------

        Result<Response> request_sync(const PreparedRequest& preq,
                                      tcp::resolver& resolver,
                                      boost::system::error_code& ec) {
            if (!ensure_connected_sync(preq.url, resolver, ec)) {
                return err_from_ec(ec);
            }

            namespace http = boost::beast::http;
            http::response<http::string_body> beast_res;

            if (!preq.url.https) {
                http::write(*m_http_stream, preq.beast_req, ec);
                if (ec) {
                    close_http();
                    return err_from_ec(ec);
                }

                m_buffer.clear();
                http::read(*m_http_stream, m_buffer, beast_res, ec);
                if (ec) {
                    close_http();
                    return err_from_ec(ec);
                }
            } else {
                http::write(*m_https_stream, preq.beast_req, ec);
                if (ec) {
                    close_https();
                    return err_from_ec(ec);
                }

                m_buffer.clear();
                http::read(*m_https_stream, m_buffer, beast_res, ec);
                if (ec) {
                    close_https();
                    return err_from_ec(ec);
                }
            }

            if (!beast_res.keep_alive()) {
                if (preq.url.https)
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

            ec = co_await ensure_connected_async(preq.url);
            if (ec) co_return err_from_ec(ec);

            namespace http = boost::beast::http;
            http::response<http::string_body> beast_res;

            if (!preq.url.https) {
                co_await http::async_write(*m_http_stream, preq.beast_req,
                                           boost::asio::redirect_error(
                                               boost::asio::use_awaitable, ec));
                if (ec) {
                    close_http();
                    co_return err_from_ec(ec);
                }

                m_buffer.clear();
                co_await http::async_read(*m_http_stream, m_buffer, beast_res,
                                          boost::asio::redirect_error(
                                              boost::asio::use_awaitable, ec));
                if (ec) {
                    close_http();
                    co_return err_from_ec(ec);
                }
            } else {
                co_await http::async_write(*m_https_stream, preq.beast_req,
                                           boost::asio::redirect_error(
                                               boost::asio::use_awaitable, ec));
                if (ec) {
                    close_https();
                    co_return err_from_ec(ec);
                }

                m_buffer.clear();
                co_await http::async_read(*m_https_stream, m_buffer, beast_res,
                                          boost::asio::redirect_error(
                                              boost::asio::use_awaitable, ec));
                if (ec) {
                    close_https();
                    co_return err_from_ec(ec);
                }
            }

            if (!beast_res.keep_alive()) {
                if (preq.url.https)
                    close_https();
                else
                    close_http();
            }

            co_return Result<Response>::ok(
                parse_beast_response(std::move(beast_res)));
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_connected_async(const UrlComponents& u) {
            Endpoint ep = endpoint_from_url(u);
            ep.normalize_default_port();
            ep.normalize_host();

            if (ep.https) co_return co_await ensure_https_connected_async(ep);
            co_return co_await ensure_http_connected_async(ep);
        }

        bool ensure_connected_sync(const UrlComponents& u,
                                   tcp::resolver& resolver,
                                   boost::system::error_code& ec) {
            Endpoint ep = endpoint_from_url(u);
            ep.normalize_default_port();
            ep.normalize_host();

            if (ep.https) {
                return ensure_https_connected_sync(ep, resolver, ec);
            }
            return ensure_http_connected_sync(ep, resolver, ec);
        }

        static Endpoint endpoint_from_url(const UrlComponents& u) {
            Endpoint ep;
            ep.host = u.host;
            ep.port = u.port;
            ep.https = u.https;
            return ep;
        }

        static Result<Response> err_from_ec(
            const boost::system::error_code& ec) {
            Error e{};
            e.code = Error::Code::NetworkError;  // adjust to your enum
            e.message = ec.message();
            return Result<Response>::err(std::move(e));
        }

        bool ensure_http_connected_sync(Endpoint ep, tcp::resolver& resolver,
                                        boost::system::error_code& ec) {
            if (m_endpoint.https) close_https();

            if (m_http_stream && m_http_stream->socket().is_open() &&
                !m_endpoint.https &&
                is_same_endpoint(m_endpoint.host, m_endpoint.port, ep.host,
                                 ep.port)) {
                ec.clear();
                return true;
            }

            close_http();
            m_endpoint = ep;

            auto results = resolver.resolve(ep.host, ep.port, ec);
            if (ec) return false;

            m_http_stream.emplace(resolver.get_executor());
            m_http_stream->connect(results, ec);
            return !ec;
        }

        bool ensure_https_connected_sync(Endpoint ep, tcp::resolver& resolver,
                                         boost::system::error_code& ec) {
            if (!m_endpoint.https && !m_endpoint.host.empty()) close_http();

            if (m_https_stream &&
                boost::beast::get_lowest_layer(*m_https_stream)
                    .socket()
                    .is_open() &&
                m_endpoint.https &&
                is_same_endpoint(m_endpoint.host, m_endpoint.port, ep.host,
                                 ep.port)) {
                ec.clear();
                return true;
            }

            close_https();
            m_endpoint = ep;

            auto results = resolver.resolve(ep.host, ep.port, ec);
            if (ec) return false;

            m_https_stream.emplace(resolver.get_executor(), m_ssl_ctx);

            boost::beast::get_lowest_layer(*m_https_stream)
                .connect(results, ec);
            if (ec) return false;

            if (!set_sni(*m_https_stream, ep.host, ec)) return false;

            auto handshake_result = m_https_stream->handshake(
                boost::asio::ssl::stream_base::client, ec);
            (void)handshake_result;  // suppress unused warning
            return !ec;
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_http_connected_async(Endpoint ep) {
            boost::system::error_code ec;

            if (m_endpoint.https) close_https();

            if (m_http_stream && m_http_stream->socket().is_open() &&
                !m_endpoint.https &&
                is_same_endpoint(m_endpoint.host, m_endpoint.port, ep.host,
                                 ep.port)) {
                co_return ec;
            }

            close_http();
            m_endpoint = ep;

            tcp::resolver resolver(m_ex);
            auto results = co_await resolver.async_resolve(
                ep.host, ep.port,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) co_return ec;

            m_http_stream.emplace(m_ex);
            co_await m_http_stream->async_connect(
                results,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            co_return ec;
        }

        boost::asio::awaitable<boost::system::error_code>
        ensure_https_connected_async(Endpoint ep) {
            boost::system::error_code ec;

            if (!m_endpoint.https && !m_endpoint.host.empty()) close_http();

            if (m_https_stream &&
                boost::beast::get_lowest_layer(*m_https_stream)
                    .socket()
                    .is_open() &&
                m_endpoint.https &&
                is_same_endpoint(m_endpoint.host, m_endpoint.port, ep.host,
                                 ep.port)) {
                co_return ec;
            }

            close_https();
            m_endpoint = ep;

            tcp::resolver resolver(m_ex);
            auto results = co_await resolver.async_resolve(
                ep.host, ep.port,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) co_return ec;

            m_https_stream.emplace(m_ex, m_ssl_ctx);
            co_await boost::beast::get_lowest_layer(*m_https_stream)
                .async_connect(results, boost::asio::redirect_error(
                                            boost::asio::use_awaitable, ec));
            if (ec) co_return ec;

            if (!set_sni(*m_https_stream, ep.host, ec)) co_return ec;

            co_await m_https_stream->async_handshake(
                boost::asio::ssl::stream_base::client,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            co_return ec;
        }

       private:
        boost::asio::any_io_executor m_ex;
        boost::asio::ssl::context& m_ssl_ctx;

        Endpoint m_endpoint{};
        boost::beast::flat_buffer m_buffer{};

        std::optional<boost::beast::tcp_stream> m_http_stream;
        std::optional<boost::beast::ssl_stream<boost::beast::tcp_stream>>
            m_https_stream;
    };

}  // namespace rest_cpp
