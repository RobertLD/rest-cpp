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
#include "rest_cpp/serialize_impl.hpp"
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

        // Templated versions of common methods
        template <typename T>
        boost::asio::awaitable<Result<T>> get(std::string url) {
            co_return to_result_t<T>(co_await get(std::move(url)));
        }

        template <typename T>
        boost::asio::awaitable<Result<T>> post(std::string url,
                                               std::string body) {
            co_return to_result_t<T>(
                co_await post(std::move(url), std::move(body)));
        }

       private:
        template <typename T>
        Result<T> to_result_t(Result<Response>&& res) {
            if (res.has_error()) {
                return Result<T>::err(res.error());
            }
            T out;
            deserialize(res.value(), out);
            return Result<T>::ok(std::move(out));
        }

        [[nodiscard]] AsyncRestClientConfiguration const& config()
            const noexcept {
            return cfg_;
        }

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
