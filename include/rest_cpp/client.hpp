#pragma once
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string>

#include "config.hpp"
#include "request.hpp"
#include "response.hpp"
#include "rest_cpp/connection/connection.hpp"
#include "rest_cpp/url.hpp"
#include "result.hpp"
#include "serialize_impl.hpp"

namespace rest_cpp {

    /**
     * @brief A synchronous REST client.
     *
     * This class provides a simple, blocking interface for making HTTP/HTTPS requests.
     * It is not thread-safe. Each thread should maintain its own RestClient instance.
     */
    class RestClient {
       public:
        /**
         * @brief Constructs a RestClient with the given configuration.
         * @param config The configuration for the client (e.g., base URL, timeout).
         */
        explicit RestClient(RestClientConfiguration config);
        ~RestClient() noexcept;

        RestClient(const RestClient&) = delete;
        RestClient& operator=(const RestClient&) = delete;

        RestClient(RestClient&&) = delete;
        RestClient& operator=(RestClient&&) = delete;

        /**
         * @brief Returns the client's configuration.
         * @return A constant reference to the configuration.
         */
        [[nodiscard]] const RestClientConfiguration& config() const noexcept;

        /**
         * @brief Sends a manual request.
         * @param request The request object containing method, URL, headers, and body.
         * @return A Result object containing the Response or an Error.
         */
        [[nodiscard]] Result<Response> send(const Request& request);

        /**
         * @brief Convenience methods for common HTTP verbs.
         * @{
         */

        /**
         * @brief Performs a GET request.
         * @param url The target URL or path (if base_url is set).
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> get(const std::string& url);

        /**
         * @brief Performs a HEAD request.
         * @param url The target URL or path.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> head(const std::string& url);

        /**
         * @brief Performs a DELETE request.
         * @param url The target URL or path.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> del(const std::string& url);

        /**
         * @brief Performs an OPTIONS request.
         * @param url The target URL or path.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> options(const std::string& url);

        /**
         * @brief Performs a POST request.
         * @param url The target URL or path.
         * @param body The request body.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> post(const std::string& url,
                                            std::string body);

        /**
         * @brief Performs a PUT request.
         * @param url The target URL or path.
         * @param body The request body.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> put(const std::string& url,
                                           std::string body);

        /**
         * @brief Performs a PATCH request.
         * @param url The target URL or path.
         * @param body The request body.
         * @return The result of the request.
         */
        [[nodiscard]] Result<Response> patch(const std::string& url,
                                             std::string body);
        /** @} */

        /**
         * @name Templated versions for automatic serialization
         * These methods automatically deserialize the JSON response body into a DTO of type T.
         * @{
         */

        /**
         * @brief Performs a GET request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> get(const std::string& url) {
            return to_result_t<T>(get(url));
        }

        /**
         * @brief Performs a HEAD request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> head(const std::string& url) {
            return to_result_t<T>(head(url));
        }

        /**
         * @brief Performs a DELETE request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> del(const std::string& url) {
            return to_result_t<T>(del(url));
        }

        /**
         * @brief Performs an OPTIONS request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> options(const std::string& url) {
            return to_result_t<T>(options(url));
        }

        /**
         * @brief Performs a POST request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @param body The request body.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> post(const std::string& url, std::string body) {
            return to_result_t<T>(post(url, std::move(body)));
        }

        /**
         * @brief Performs a PUT request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @param body The request body.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> put(const std::string& url, std::string body) {
            return to_result_t<T>(put(url, std::move(body)));
        }

        /**
         * @brief Performs a PATCH request and deserializes the response.
         * @tparam T The type to deserialize into.
         * @param url The target URL or path.
         * @param body The request body.
         * @return A Result containing the deserialized object or an Error.
         */
        template <typename T>
        [[nodiscard]] Result<T> patch(const std::string& url,
                                      std::string body) {
            return to_result_t<T>(patch(url, std::move(body)));
        }
        /** @} */

       private:
        template <typename T>
        Result<T> to_result_t(Result<Response>&& res) {
            if (res.has_error()) {
                return Result<T>::err(res.error());
            }
            T out;
            // Lookup deserialize via ADL or from included headers
            deserialize(res.value(), out);
            return Result<T>::ok(std::move(out));
        }

        RestClientConfiguration m_config{};
        std::optional<UrlComponents> m_base_url;

        boost::asio::io_context io_{1};
        tcp::resolver m_resolver{io_};

        boost::asio::ssl::context m_ssl_context{
            boost::asio::ssl::context::tls_client};

        std::optional<Connection<Mode::Sync>> m_conn;
        std::optional<Endpoint> m_conn_endpoint;

        void ensure_connection_for(const Endpoint& ep);

        static Endpoint endpoint_from_url(const UrlComponents& u) {
            Endpoint ep;
            ep.host = u.host;
            ep.port = u.port;
            ep.https = u.https;
            return ep;
        }

        [[nodiscard]] inline Result<UrlComponents> resolve_request_url(
            std::string_view url) const {
            const rest_cpp::UrlComponents* base =
                m_base_url ? &*m_base_url : nullptr;
            return rest_cpp::url_utils::resolve_url(url, base);
        }
    };

}  // namespace rest_cpp
