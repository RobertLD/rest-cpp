#include "rest_cpp/client.hpp"

#include <boost/beast/http/verb.hpp>

#include "rest_cpp/endpoint.hpp"
#include "rest_cpp/url.hpp"

namespace beast = boost::beast;
namespace http = beast::http;

namespace rest_cpp {

    RestClient::RestClient(RestClientConfiguration config)
        : m_config(std::move(config)),
          io_(1),
          m_resolver(io_),
          m_ssl_context(boost::asio::ssl::context::tls_client),
          m_conn(io_.get_executor(), m_ssl_context) {
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
        m_conn.close_http();
        m_conn.close_https();
    }

    const RestClientConfiguration& RestClient::config() const noexcept {
        return m_config;
    }

    Result<Response> RestClient::send(const Request& request) {
        // Resolve URL (fast path for relative, parse for absolute)
        auto u_res = resolve_request_url(request.url);
        if (u_res.has_error()) {
            return Result<Response>::err(u_res.error());
        }

        rest_cpp::UrlComponents parsed_url = std::move(u_res.value());

        // Validate verb like before
        const http::verb verb = rest_cpp::to_boost_http_method(request.method);
        if (verb == http::verb::unknown) {
            return Result<Response>::err(
                Error{Error::Code::Unknown, "Unknown HTTP method"});
        }

        // Build PreparedRequest for shared connection code
        PreparedRequest preq;
        preq.url = std::move(parsed_url);
        preq.beast_req = prepare_beast_request(
            request, preq.url, m_config.user_agent, /*keep_alive=*/true);

        boost::system::error_code ec;
        // NOTE: m_conn.request() handles connect + write + read + keepalive
        // close logic.
        return m_conn.request(preq, m_resolver, ec);
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
