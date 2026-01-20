#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <optional>
#include <string_view>

#include "rest_cpp/config.hpp"
#include "rest_cpp/connection/connection_pool.hpp"
#include "rest_cpp/request.hpp"
#include "rest_cpp/response.hpp"
#include "rest_cpp/result.hpp"
#include "rest_cpp/url.hpp"

namespace rest_cpp {

    class AsyncRestClient {
       public:
        explicit AsyncRestClient(boost::asio::any_io_executor ex,
                                 AsyncRestClientConfiguration cfg);

        boost::asio::awaitable<Result<Response>> send(Request request);

        boost::asio::awaitable<Result<Response>> get(std::string url);
        boost::asio::awaitable<Result<Response>> post(std::string url,
                                                      std::string body);

        // add others similarly

        AsyncRestClientConfiguration const& config() const noexcept {
            return cfg_;
        }

       private:
        using tcp = boost::asio::ip::tcp;

        Result<UrlComponents> resolve_request_url(std::string_view url) const;

        AsyncRestClientConfiguration cfg_;
        std::optional<UrlComponents> base_url_;

        boost::asio::any_io_executor ex_;
        tcp::resolver resolver_;

        boost::asio::ssl::context ssl_ctx_;

        std::shared_ptr<ConnectionPool> pool_;
    };

}  // namespace rest_cpp
