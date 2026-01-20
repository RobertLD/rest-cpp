#include "rest_cpp/async_client.hpp"

#include <boost/beast/http/verb.hpp>

#include "rest_cpp/connection/connection_pool.hpp"
#include "rest_cpp/endpoint.hpp"
#include "rest_cpp/url.hpp"

namespace beast = boost::beast;
namespace http = beast::http;

namespace rest_cpp {

    AsyncRestClient::AsyncRestClient(boost::asio::any_io_executor ex,
                                     AsyncRestClientConfiguration cfg)
        : cfg_(std::move(cfg)),
          ex_(std::move(ex)),
          resolver_(ex_),
          ssl_ctx_(boost::asio::ssl::context::tls_client),
          pool_(std::make_shared<ConnectionPool>(ex_, ssl_ctx_,
                                                 cfg_.pool_config)) {
        init_tls_on_ssl_context(ssl_ctx_);

        ssl_ctx_.set_verify_mode(cfg_.verify_tls
                                     ? boost::asio::ssl::verify_peer
                                     : boost::asio::ssl::verify_none);

        if (cfg_.base_url) {
            auto base_res = rest_cpp::url_utils::parse_base_url(*cfg_.base_url);
            if (base_res.has_error()) {
                throw std::runtime_error("Invalid base_url: " +
                                         base_res.error().message);
            }
            base_url_ = std::move(base_res.value());
        }
    }

    Result<UrlComponents> AsyncRestClient::resolve_request_url(
        std::string_view url) const {
        const rest_cpp::UrlComponents* base = base_url_ ? &*base_url_ : nullptr;
        return rest_cpp::url_utils::resolve_url(url, base);
    }

    boost::asio::awaitable<Result<Response>> AsyncRestClient::send(
        Request request) {
        // Resolve URL
        auto u_res = resolve_request_url(request.url);
        if (u_res.has_error()) {
            co_return Result<Response>::err(u_res.error());
        }

        UrlComponents u = std::move(u_res.value());

        // Validate verb
        if (rest_cpp::to_boost_http_method(request.method) ==
            http::verb::unknown) {
            co_return Result<Response>::err(
                Error{Error::Code::Unknown, "Unknown HTTP method"});
        }

        // Build normalized endpoint
        Endpoint ep;
        ep.host = u.host;
        ep.port = u.port;
        ep.https = u.https;
        ep.normalize_default_port();
        ep.normalize_host();

        // Build PreparedRequest (endpoint-only)
        PreparedRequest preq;
        preq.ep = ep;
        preq.beast_req = prepare_beast_request(request, u, cfg_.user_agent,
                                               /*keep_alive=*/true);

        // Acquire from pool (this may wait if pool limits are hit)
        auto lease_result = co_await pool_->acquire(ep);
        if (lease_result.has_error()) {
            co_return Result<Response>::err(lease_result.error());
        }

        auto lease = std::move(lease_result.value());
        if (!lease) {
            co_return Result<Response>::err(
                Error{Error::Code::NetworkError, "Connection pool shutdown"});
        }

        boost::system::error_code ec;
        // Execute request; connection returned to pool when lease destructs
        co_return co_await lease->request(preq, resolver_, ec);
    }

    boost::asio::awaitable<Result<Response>> AsyncRestClient::get(
        std::string url) {
        Request r{HttpMethod::Get, std::move(url), {}, std::nullopt};
        co_return co_await send(std::move(r));
    }

    boost::asio::awaitable<Result<Response>> AsyncRestClient::post(
        std::string url, std::string body) {
        Request r{HttpMethod::Post, std::move(url), {}, std::move(body)};
        co_return co_await send(std::move(r));
    }

}  // namespace rest_cpp
