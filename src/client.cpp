#include "rest_cpp/client.hpp"

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/system_error.hpp>

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
          ssl_ctx_(boost::asio::ssl::context::tls_client) {
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
        try {
            ssl_ctx_.set_default_verify_paths();
        } catch (const boost::system::system_error& e) {
            throw std::runtime_error(
                std::string("Failed to set default verify paths: ") + e.what());
        }

        /// Configure verification mode + throw out the old one
        static_cast<void>(
            ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer));
    }

    const RestClientConfiguration& RestClient::config() const noexcept {
        return m_config;
    }

    // Keep your send() implementation here (or stub it for now).
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
        http::request<http::string_body> req;
        req.version(11);
        req.method(verb);
        req.target(u.target);

        req.set(http::field::host, u.host);
        req.set(http::field::user_agent, m_config.user_agent);

        // Keep-alive by default
        req.keep_alive(true);

        apply_request_headers(request.headers, req.base());
        // Optional body
        if (request.body.has_value()) {
            req.body() = *request.body;
            req.prepare_payload();
        }
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

            Response out;
            out.status_code = static_cast<int>(res.result_int());
            copy_response_headers(res.base(), out.headers);
            out.body = std::move(res.body());
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

        Response out;
        out.status_code = static_cast<int>(res.result_int());
        copy_response_headers(res.base(), out.headers);
        out.body = std::move(res.body());
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
            m_host == u.host && m_port == u.port) {
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

        m_https_stream.emplace(io_, ssl_ctx_);

        // SNI for TLS
        if (!SSL_set_tlsext_host_name(m_https_stream->native_handle(),
                                      u.host.c_str())) {
            ec = boost::system::error_code(static_cast<int>(::ERR_get_error()),
                                           net::error::get_ssl_category());
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
