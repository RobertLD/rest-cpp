#include "rest_cpp/client.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/system_error.hpp>

#include "rest_cpp/connection.hpp"
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
          m_ssl_context(boost::asio::ssl::context::tls_client) {
        init_tls();
    }

    RestClient::~RestClient() noexcept {
        // Destructor must never throw.
        boost::system::error_code ec;

        // Close HTTPS first
        if (m_https_stream) {
            m_https_stream->shutdown(ec);

            // Close underlying TCP socket.
            auto r =
                beast::get_lowest_layer(*m_https_stream).socket().close(ec);

            // Stop bitching
            (void)r;

            m_https_stream.reset();
        }

        // Close HTTP.
        if (m_http_stream) {
            m_http_stream->socket().close();
            m_http_stream.reset();
        }

        m_active_https_connection = false;
        m_host.clear();
        m_port.clear();
    }

    void RestClient::init_tls() {
        // Load system default CA certificates
        init_tls_on_ssl_context(m_ssl_context);
    }

    const RestClientConfiguration& RestClient::config() const noexcept {
        return m_config;
    }

    Result<Response> RestClient::send(const Request& request) {
        // If base_url is missing, only absolute request.url is allowed
        const std::string_view base = m_config.base_url
                                          ? std::string_view(*m_config.base_url)
                                          : std::string_view{};

        // Combine base_url and request.url
        auto abs_url_res =
            rest_cpp::url_utils::combine_base_and_uri(base, request.url);

        // Check for errors in URL combination
        if (abs_url_res.has_error()) {
            return Result<Response>::err(abs_url_res.error());
        }
        // Parse the absolute URL into a ParsedUrl struct
        auto parsed_res = rest_cpp::parse_url(abs_url_res.value());
        if (parsed_res.has_error()) {
            return Result<Response>::err(parsed_res.error());
        }
        const rest_cpp::ParsedUrl& u = parsed_res.value();
        const http::verb verb = rest_cpp::to_boost_http_method(request.method);

        // IF we don't know the verb, return an error
        if (verb == http::verb::unknown) {
            return Result<Response>::err(
                Error{Error::Code::Unknown, "Unknown HTTP method"});
        }
        // Build request
        http::request<http::string_body> req =
            prepare_beast_request(request, u, m_config.user_agent);

        boost::system::error_code ec;

        // Establish the connection and do work
        if (!u.https) {
            if (!ensure_http_connected(u, ec)) {
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

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(*m_http_stream, buffer, res, ec);
            if (ec) {
                close_http();
                return Result<Response>::err(Error{
                    Error::Code::ReceiveFailed,
                    "Read failed: " + ec.message(),
                });
            }

            Response out = parse_beast_response(res);
            return Result<Response>::ok(std::move(out));
        }

        // HTTPS
        if (!ensure_https_connected(u, ec)) {
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
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(*m_https_stream, buffer, res, ec);
        if (ec) {
            close_https();
            return Result<Response>::err(Error{
                Error::Code::ReceiveFailed,
                "Read failed: " + ec.message(),
            });
        }

        Response out = parse_beast_response(res);
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
        if (m_http_stream) {
            m_http_stream->socket().close();
            m_http_stream.reset();
        }
        m_host.clear();
        m_port.clear();
    }

    bool RestClient::ensure_http_connected(const ParsedUrl& u,
                                           boost::system::error_code& ec) {
        ec = {};

        // If same endpoint and socket open, reuse.

        if (m_http_stream && m_http_stream->socket().is_open() &&
            is_same_endpoint(m_host, u.host, m_port, u.port)) {
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

        m_host = u.host;
        m_port = u.port;
        return true;
    }

    void RestClient::close_https() noexcept {
        if (m_https_stream) {
            // Best effort TLS shutdown (ignore errors).
            m_https_stream->shutdown();
            beast::get_lowest_layer(*m_https_stream).socket().close();
            m_https_stream.reset();
        }

        // If this connection was the active one, clear endpoint tracking too.
        if (m_active_https_connection) {
            m_active_https_connection = false;
            m_host.clear();
            m_port.clear();
        }
    }

    bool RestClient::ensure_https_connected(const ParsedUrl& u,
                                            boost::system::error_code& ec) {
        ec = {};

        // Reuse if same endpoint, scheme matches, and socket open.
        if (m_https_stream && m_active_https_connection && m_host == u.host &&
            m_port == u.port &&
            beast::get_lowest_layer(*m_https_stream).socket().is_open()) {
            return true;
        }

        // If we had an HTTP connection tracked as active, close it when
        // switching schemes.
        if (!m_host.empty() && !m_active_https_connection) {
            close_http();
        }
        close_https();

        auto results = m_resolver.resolve(u.host, u.port, ec);
        if (ec) return false;

        m_https_stream.emplace(io_, m_ssl_context);

        // SNI for TLS
        boost::system::error_code sni_ec;
        set_sni(*m_https_stream, u.host, ec);
        if (!rest_cpp::set_sni(*m_https_stream, u.host, sni_ec)) {
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

        m_active_https_connection = true;
        m_host = u.host;
        m_port = u.port;
        return true;
    }

}  // namespace rest_cpp
