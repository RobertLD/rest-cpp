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
          ssl_ctx_(boost::asio::ssl::context::tls_client) {
        init_tls();
    }

    RestClient::~RestClient() = default;

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

        apply_request_headers(request.headers, req.base());
        // Optional body
        if (request.body.has_value()) {
            req.body() = *request.body;
            req.prepare_payload();
        }
        boost::system::error_code ec;

        // Resolve
        tcp::resolver resolver(io_);
        auto results = resolver.resolve(u.host, u.port, ec);
        if (ec) {
            return Result<Response>::err(Error{
                Error::Code::ConnectionFailed,
                "Resolve failed: " + ec.message(),
            });
        }
        // Establish the connection and do work
        if (!u.https) {
            // Plain HTTP
            beast::tcp_stream stream(io_);

            stream.connect(results, ec);
            if (ec) {
                return Result<Response>::err(Error{
                    Error::Code::ConnectionFailed,
                    "Connect failed: " + ec.message(),
                });
            }

            // Send the HTTP request to the remote host
            http::write(stream, req, ec);
            if (ec) {
                return Result<Response>::err(Error{
                    Error::Code::SendFailed,
                    "Write failed: " + ec.message(),
                });
            }

            // Receive the HTTP response
            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res, ec);
            if (ec) {
                return Result<Response>::err(Error{
                    Error::Code::ReceiveFailed,
                    "Read failed: " + ec.message(),
                });
            }

            // Best-effort shutdown
            stream.socket().shutdown(tcp::socket::shutdown_both);

            Response out;
            out.status_code = static_cast<int>(res.result_int());
            copy_response_headers(res.base(), out.headers);
            out.body = std::move(res.body());
            return Result<Response>::ok(std::move(out));
        }

        // HTTPS
        beast::ssl_stream<beast::tcp_stream> stream(io_, ssl_ctx_);
        // SNI - set the hostname for SSL handshake
        if (!SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str())) {
            boost::system::error_code sni_ec(
                static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category());

            return Result<Response>::err(Error{
                Error::Code::TlsHandshakeFailed,
                "SNI set failed: " + sni_ec.message(),
            });
        }
        beast::get_lowest_layer(stream).connect(results, ec);
        if (ec) {
            return Result<Response>::err(Error{
                Error::Code::ConnectionFailed,
                "Connect failed: " + ec.message(),
            });
        }
        stream.handshake(ssl::stream_base::client, ec);
        if (ec) {
            return Result<Response>::err(Error{
                Error::Code::TlsHandshakeFailed,
                "TLS handshake failed: " + ec.message(),
            });
        }
        http::write(stream, req, ec);
        if (ec) {
            return Result<Response>::err(Error{
                Error::Code::SendFailed,
                "Write failed: " + ec.message(),
            });
        }
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res, ec);
        if (ec) {
            return Result<Response>::err(Error{
                Error::Code::ReceiveFailed,
                "Read failed: " + ec.message(),
            });
        }
        // TLS shutdown
        stream.shutdown(ec);
        if (ec == net::error::eof || ec == ssl::error::stream_truncated) {
            ec = {};
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

}  // namespace rest_cpp
