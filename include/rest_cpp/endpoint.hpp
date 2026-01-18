#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>

namespace rest_cpp {

    struct Endpoint {
        std::string host;
        std::string port;
        bool https;

        void clear() {
            host.clear();
            port.clear();
            https = false;
        }

        inline void normalize_default_port() {
            if (port.empty()) port = https ? "443" : "80";
        }

        inline void normalize_host() {
            if (host.empty()) host = "localhost";
            std::transform(host.begin(), host.end(), host.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        friend bool operator==(Endpoint const& a, Endpoint const& b) noexcept {
            return a.https == b.https && a.host == b.host && a.port == b.port;
        }
    };

    inline constexpr bool is_same_endpoint(const std::string_view cur_host,
                                           const std::string_view cur_port,
                                           const std::string_view new_host,
                                           const std::string_view new_port) {
        return cur_host == new_host && cur_port == new_port;
    }

    inline void update_endpoint(std::string& cur_host, std::string& cur_port,
                                const std::string& new_host,
                                const std::string& new_port) {
        cur_host = new_host;
        cur_port = new_port;
    }

    inline bool set_sni(
        boost::beast::ssl_stream<boost::beast::tcp_stream>& stream,
        const std::string& host, boost::system::error_code& ec) {
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            ec = boost::system::error_code(
                static_cast<int>(::ERR_get_error()),
                boost::asio::error::get_ssl_category());
            return false;
        }
        return true;
    }

    void inline init_tls_on_ssl_context(
        boost::asio::ssl::context&
            ssl_context) {  // Load system default CA certificates
        try {
            ssl_context.set_default_verify_paths();
        } catch (const boost::system::system_error& e) {
            throw std::runtime_error(
                std::string("Failed to set default verify paths: ") + e.what());
        }

        /// Configure verification mode + throw out the old one
        static_cast<void>(
            ssl_context.set_verify_mode(boost::asio::ssl::verify_peer));
    }

}  // namespace rest_cpp

namespace std {
    template <>
    struct hash<rest_cpp::Endpoint> {
        size_t operator()(rest_cpp::Endpoint const& e) const noexcept {
            // Any stable combination works; keep it fast.
            size_t h = 1469598103934665603ull;
            auto mix = [&](std::string_view s) {
                for (unsigned char c : s) {
                    h ^= c;
                    h *= 1099511628211ull;
                }
            };
            h ^= static_cast<size_t>(e.https);
            h *= 1099511628211ull;
            mix(e.host);
            mix(e.port);
            return h;
        }
    };
}  // namespace std