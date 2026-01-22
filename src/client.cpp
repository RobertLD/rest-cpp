#include "rest_cpp/client.hpp"

#include <boost/beast/http/verb.hpp>

#include "rest_cpp/endpoint.hpp"
#include "rest_cpp/middleware.hpp"
#include "rest_cpp/url.hpp"

namespace beast = boost::beast;
namespace http = beast::http;

namespace rest_cpp {

    void RestClient::ensure_connection_for(const Endpoint& ep) {
        if (m_conn && m_conn_endpoint && (*m_conn_endpoint == ep)) {
            return;
        }

        m_conn.reset();
        m_conn_endpoint = ep;
        m_conn.emplace(io_.get_executor(), m_ssl_context, ep);
    }

    RestClient::RestClient(RestClientConfiguration config)
        : m_config(std::move(config)),
          io_(1),
          m_resolver(io_),
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
        if (m_conn) {
            m_conn->close_http();
            m_conn->close_https();
        }
        m_conn.reset();
        m_conn_endpoint.reset();
    }

    const RestClientConfiguration& RestClient::config() const noexcept {
        return m_config;
    }

    Result<Response> RestClient::send(const Request& request) {
        Request req_copy = request;

        // Resolve URL (relative vs absolute, base_url handling)
        auto u_res = resolve_request_url(req_copy.url);
        if (u_res.has_error()) {
            return Result<Response>::err(u_res.error());
        }

        UrlComponents u = std::move(u_res.value());

        // Apply interceptors
        if (!m_config.interceptors.empty()) {
            for (const auto& interceptor : m_config.interceptors) {
                if (interceptor) {
                    interceptor->prepare(req_copy, u);
                }
            }
        }

        // Validate verb like before
        const http::verb verb = rest_cpp::to_boost_http_method(req_copy.method);
        if (verb == http::verb::unknown) {
            return Result<Response>::err(
                Error{Error::Code::Unknown, "Unknown HTTP method"});
        }

        // Build normalized endpoint from URL
        Endpoint ep = RestClient::endpoint_from_url(u);

        // Ensure we have a connection bound to this endpoint
        ensure_connection_for(ep);

        // Build PreparedRequest (no UrlComponents stored inside)
        PreparedRequest preq;
        preq.ep = ep;
        preq.beast_req = prepare_beast_request(req_copy, u, m_config.user_agent,
                                               /*keep_alive=*/true);

        boost::system::error_code ec;
        return m_conn->request(preq, m_resolver, ec);
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
