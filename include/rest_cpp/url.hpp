#pragma once

#include <string>

#include "result.hpp"

namespace rest_cpp {
    struct ParsedUrl {
        bool https{false};
        std::string host;
        std::string port;
        std::string target;
    };

    namespace url_utils {
        /// @brief Check if a URL is an absolute HTTP or HTTPS URL.
        /// @param s The URL string to check.
        /// @return True if the URL starts with "http://" or "https://".
        inline bool is_absolute_url_with_protocol(std::string_view s) {
            return (s.rfind("https://", 0) == 0) ||
                   (s.rfind("http://", 0) == 0);
        }

        /// @brief Trim trailing slashes from a string.
        /// @param s The string to trim.
        /// @return The trimmed string.
        inline std::string trim_trailing_slashes(std::string s) {
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }

        inline Result<std::string> combine_base_and_uri(
            std::string_view base_url, std::string_view uri_or_url) {
            auto make_err = [&](std::string msg) -> Result<std::string> {
                Error e{};
                e.message = std::move(msg);
                e.code = Error::Code::InvalidUrl;
                return Result<std::string>::err(std::move(e));
            };

            // If absolute, return that bitch
            if (is_absolute_url_with_protocol(uri_or_url)) {
                return Result<std::string>::ok(std::string(uri_or_url));
            }

            if (base_url.empty()) {
                return make_err("Relative URI provided but base_url is empty");
            }
            if (!is_absolute_url_with_protocol(base_url)) {
                return make_err("base_url must start with http:// or https://");
            }

            std::string base = trim_trailing_slashes(std::string(base_url));

            // Interpret empty uri as "/"
            if (uri_or_url.empty()) {
                return Result<std::string>::ok(base + "/");
            }

            // If uri has no leading slash, insert one.
            if (uri_or_url.front() != '/') {
                return Result<std::string>::ok(base + "/" +
                                               std::string(uri_or_url));
            }

            // Normal case: base + "/path"
            return Result<std::string>::ok(base + std::string(uri_or_url));
        }

    }  // namespace url_utils

    /// @brief Parse a URL into its components.
    /// @param url The URL string to parse.
    /// @return A Result containing the ParsedUrl on success, or an Error on
    /// failure.
    inline Result<ParsedUrl> parse_url(const std::string& url) {
        auto make_err = [&](std::string msg) -> Result<ParsedUrl> {
            Error e{};
            e.message = std::move(msg);
            e.code = Error::Code::InvalidUrl;  // adjust to your Error scheme
            return Result<ParsedUrl>::err(std::move(e));
        };

        std::string_view s(url);

        bool https = false;
        if (s.rfind("https://", 0) == 0) {
            https = true;
            s.remove_prefix(std::string_view("https://").size());
        } else if (s.rfind("http://", 0) == 0) {
            https = false;
            s.remove_prefix(std::string_view("http://").size());
        } else {
            return make_err("URL must start with http:// or https://");
        }

        // Split host[:port] from path
        std::string_view hostport = s;
        std::string_view path = "/";
        if (auto slash = s.find('/'); slash != std::string_view::npos) {
            hostport = s.substr(0, slash);
            path = s.substr(slash);
        }

        if (hostport.empty()) {
            return make_err("URL missing host");
        }

        std::string host;
        std::string port;

        // Port parsing (simple: last ':' splits host/port)
        if (auto colon = hostport.rfind(':'); colon != std::string_view::npos) {
            host = std::string(hostport.substr(0, colon));
            port = std::string(hostport.substr(colon + 1));
            if (port.empty()) {
                return make_err("URL has empty port");
            }
        } else {
            host = std::string(hostport);
            port = https ? "443" : "80";
        }

        if (host.empty()) {
            return make_err("URL has empty host");
        }

        ParsedUrl out;
        out.https = https;
        out.host = std::move(host);
        out.port = std::move(port);
        out.target = path.empty() ? "/" : std::string(path);
        return Result<ParsedUrl>::ok(std::move(out));
    }

}  // namespace rest_cpp