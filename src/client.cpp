#include "rest_cpp/client.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/system_error.hpp>

#include "rest_cpp/endpoint.hpp"
#include "rest_cpp/url.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace rest_cpp {

    RestClient::RestClient(RestClientConfiguration config)
        : m_config(std::move(config)),
          io_(1),
          m_resolver(io_),
          m_buffer(),
          m_ssl_context(boost::asio::ssl::context::tls_client) {
        init_tls_on_ssl_context(m_ssl_context);
        if (m_config.base_url) {
            auto base_res =
                rest_cpp::url_utils::parse_base_url(*m_config.base_url);
            if (base_res.has_error()) {
                throw std::runtime_error("Invalid base_url: " +
                                         base_res.error().message);
            }
            m_base_url = std::move(base_res.value());
        }
    }

    RestClient::~RestClient() noexcept {
        boost::system::error_code ec;

        // Close HTTPS first
        if (m_https_stream) {
            auto shutdown_result = m_https_stream->shutdown(ec);
            (void)shutdown_result;  // stop bitching hoe

            // Close underlying TCP socket.
            auto r =
                beast::get_lowest_layer(*m_https_stream).socket().close(ec);

            // Stop bitching
            (void)r;

            m_https_stream.reset();
        }

        // Close HTTP.
        if (m_http_stream) {
            auto close_result = m_http_stream->socket().close(ec);
            (void)close_result;  // stop bitching hoe
            m_http_stream.reset();
        }

        m_connection_details.https = false;
        m_connection_details.clear();
    }

    const RestClientConfiguration& RestClient::config() const noexcept {
        return m_config;
    }

    Result<Response> RestClient::send(const Request& request) {
        // Resolve URL (fast path for relative, parse for absolute)
        const rest_cpp::UrlComponents* base =
            m_base_url ? &*m_base_url : nullptr;

        auto u_res = rest_cpp::url_utils::resolve_url(request.url, base);
        if (u_res.has_error()) {
            return Result<Response>::err(u_res.error());
        }

        rest_cpp::UrlComponents parsed_url = std::move(u_res.value());
        const http::verb verb = rest_cpp::to_boost_http_method(request.method);

        // IF we don't know the verb, return an error
        if (verb == http::verb::unknown) {
            return Result<Response>::err(
                Error{Error::Code::Unknown, "Unknown HTTP method"});
        }
        // Build request
        http::request<http::string_body> req =
            prepare_beast_request(request, parsed_url, m_config.user_agent);
        boost::system::error_code ec;

        // Establish the connection and do work
        if (!parsed_url.https) {
            if (!ensure_http_connected(parsed_url, ec)) {
                return Result<Response>::err(Error{
                    Error::Code::ConnectionFailed,
                    "Connect failed: " + ec.message(),
                });
            }

            http::write(*m_http_stream, req, ec);
            if (ec) {
                close_http();
                return Result<Response>::err(Error{
                    Error::Code::SendFailed,
                    "Write failed: " + ec.message(),
                });
            }

            m_buffer.consume(m_buffer.size());  // clear but keep capacity
            http::response_parser<http::string_body> parser;
            parser.body_limit(m_config.max_body_bytes);

            http::read(*m_http_stream, m_buffer, parser, ec);
            if (ec) {
                close_http();
                return Result<Response>::err(Error{
                    Error::Code::ReceiveFailed,
                    "Read failed: " + ec.message(),
                });
            }

            Response out = parse_beast_response(std::move(parser.get()));
            return Result<Response>::ok(std::move(out));
        }

        // HTTPS
        if (!ensure_https_connected(parsed_url, ec)) {
            return Result<Response>::err(Error{
                Error::Code::ConnectionFailed,
                "Connect failed: " + ec.message(),
            });
        }
        http::write(*m_https_stream, req, ec);
        if (ec) {
            close_https();
            return Result<Response>::err(Error{
                Error::Code::SendFailed,
                "Write failed: " + ec.message(),
            });
        }
        m_buffer.consume(m_buffer.size());  // clear but keep capacity
        http::response_parser<http::string_body> parser;
        parser.body_limit(m_config.max_body_bytes);
        http::read(*m_https_stream, m_buffer, parser, ec);
        if (ec) {
            close_https();
            return Result<Response>::err(Error{
                Error::Code::ReceiveFailed,
                "Read failed: " + ec.message(),
            });
        }

        Response out = parse_beast_response(std::move(parser.get()));
        return Result<Response>::ok(std::move(out));
    }

    // Convenience methods
    Result<Response> RestClient::get(const std::string& url) {
        Request r{HttpMethod::Get, url, {}, std::nullopt};
        return send(r);
    }

    Result<Response> RestClient::head(const std::string& url) {
        Request r{HttpMethod::Head, url, {}, std::nullopt};
        return send(r);
    }

    Result<Response> RestClient::del(const std::string& url) {
        Request r{HttpMethod::Delete, url, {}, std::nullopt};
        return send(r);
    }

    Result<Response> RestClient::options(const std::string& url) {
        Request r{HttpMethod::Options, url, {}, std::nullopt};
        return send(r);
    }

    Result<Response> RestClient::post(const std::string& url,
                                      std::string body) {
        Request r{HttpMethod::Post, url, {}, std::move(body)};
        return send(r);
    }

    Result<Response> RestClient::put(const std::string& url, std::string body) {
        Request r{HttpMethod::Put, url, {}, std::move(body)};
        return send(r);
    }

    Result<Response> RestClient::patch(const std::string& url,
                                       std::string body) {
        Request r{HttpMethod::Patch, url, {}, std::move(body)};
        return send(r);
    }

    void RestClient::close_http() noexcept {
        boost::system::error_code ec;
        if (m_http_stream) {
            auto close_result = m_http_stream->socket().close(ec);
            (void)close_result;  // stop bitching hoe
            m_http_stream.reset();
        }
        m_connection_details.clear();
    }

    bool RestClient::ensure_http_connected(const UrlComponents& u,
                                           boost::system::error_code& ec) {
        ec = {};

        // If same endpoint and socket open, reuse.

        if (m_http_stream && m_http_stream->socket().is_open() &&
            is_same_endpoint(m_connection_details.host,
                             m_connection_details.port, u.host, u.port)) {
            return true;
        }

        // Endpoint changed or dead socket: rebuild.
        close_http();

        auto results = m_resolver.resolve(u.host, u.port, ec);
        if (ec) return false;

        m_http_stream.emplace(io_);
        m_http_stream->connect(results, ec);
        if (ec) {
            close_http();
            return false;
        }

        m_connection_details.host = u.host;
        m_connection_details.port = u.port;
        return true;
    }

    void RestClient::close_https() noexcept {
        boost::system::error_code ec;

        if (m_https_stream) {
            // Best effort TLS shutdown (ignore errors).
            m_https_stream->shutdown(ec);
            // These are common/non-fatal during TLS shutdown.
            if (ec == net::error::eof || ec == ssl::error::stream_truncated) {
                ec.clear();
            }
            auto close_result =
                beast::get_lowest_layer(*m_https_stream).socket().close(ec);
            (void)close_result;  // stop bitching hoe
            m_https_stream.reset();
        }

        // If this connection was the active one, clear endpoint tracking too.
        if (m_connection_details.https) {
            m_connection_details.https = false;
            m_connection_details.host.clear();
            m_connection_details.port.clear();
        }
    }

    bool RestClient::ensure_https_connected(const UrlComponents& u,
                                            boost::system::error_code& ec) {
        ec = {};

        // Reuse if same endpoint, scheme matches, and socket open.
        if (m_https_stream && m_connection_details.https &&
            m_connection_details.host == u.host &&
            m_connection_details.port == u.port &&
            beast::get_lowest_layer(*m_https_stream).socket().is_open()) {
            return true;
        }

        // If we had an HTTP connection tracked as active, close it when
        // switching schemes.
        if (!m_connection_details.host.empty() && !m_connection_details.https) {
            close_http();
        }
        close_https();

        auto results = m_resolver.resolve(u.host, u.port, ec);
        if (ec) return false;

        m_https_stream.emplace(io_, m_ssl_context);

        // SNI for TLS
        boost::system::error_code sni_ec;
        if (!set_sni(*m_https_stream, u.host, sni_ec)) {
            ec = sni_ec;
            close_https();
            return false;
        }

        beast::get_lowest_layer(*m_https_stream).connect(results, ec);
        if (ec) {
            close_https();
            return false;
        }

        m_https_stream->handshake(ssl::stream_base::client, ec);
        if (ec) {
            close_https();
            return false;
        }

        m_connection_details.https = true;
        m_connection_details.host = u.host;
        m_connection_details.port = u.port;
        return true;
    }

}  // namespace rest_cpp
