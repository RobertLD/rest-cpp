#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/system/error_code.hpp>

namespace rest_cpp {
    inline bool is_same_endpoint(const std::string& cur_host,
                                 const std::string& cur_port,
                                 const std::string& new_host,
                                 const std::string& new_port) {
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