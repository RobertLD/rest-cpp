#pragma once

#include <string>
#include <string_view>

#include "result.hpp"

namespace rest_cpp {

    struct UrlComponents {
        bool https{false};
        std::string host;
        std::string port;
        // For a parsed absolute URL: full target (path + optional query).
        // For a parsed base URL (via parse_base_url): normalized prefix path
        // ("" or "/api") For a resolved URL: full request target.
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

        /// @brief Join a base URL and a relative URI (kept for compatibility /
        /// non-hot paths).
        inline Result<std::string> combine_base_and_uri(
            std::string_view base_url, std::string_view uri_or_url) {
            auto make_err = [&](std::string msg) -> Result<std::string> {
                Error e{};
                e.message = std::move(msg);
                e.code = Error::Code::InvalidUrl;
                return Result<std::string>::err(std::move(e));
            };

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

            if (uri_or_url.empty()) {
                return Result<std::string>::ok(base + "/");
            }
            if (uri_or_url.front() != '/') {
                return Result<std::string>::ok(base + "/" +
                                               std::string(uri_or_url));
            }
            return Result<std::string>::ok(base + std::string(uri_or_url));
        }

        /// @brief Parse a base_url into components suitable for resolving
        /// relative targets. The returned UrlComponents.target is a *normalized
        /// prefix*:
        /// - "/" becomes ""
        /// - trailing '/' removed
        /// - query is rejected (to keep prefix joining simple/fast)
        inline Result<UrlComponents> parse_base_url(std::string_view base_url);

        /// @brief Resolve a request URL (absolute or relative) into
        /// UrlComponents.
        /// - If uri_or_url is absolute => parses it (slow path)
        /// - If relative => requires base (parsed via parse_base_url), then
        /// joins prefix + relative
        inline Result<UrlComponents> resolve_url(
            std::string_view uri_or_url,
            const UrlComponents* base /*nullable*/);

    }  // namespace url_utils

    /// @brief Parse an absolute URL into its components.
    /// @param url The URL string to parse.
    /// @return A Result containing the UrlComponents on success, or an Error on
    /// failure.
    inline Result<UrlComponents> parse_url(const std::string& url) {
        auto make_err = [&](std::string msg) -> Result<UrlComponents> {
            Error e{};
            e.message = std::move(msg);
            e.code = Error::Code::InvalidUrl;
            return Result<UrlComponents>::err(std::move(e));
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

        UrlComponents out;
        out.https = https;
        out.host = std::move(host);
        out.port = std::move(port);
        out.target = path.empty() ? "/" : std::string(path);
        return Result<UrlComponents>::ok(std::move(out));
    }

    // ---- url_utils implementations ----

    namespace url_utils {

        inline Result<UrlComponents> parse_base_url(std::string_view base_url) {
            auto make_err = [&](std::string msg) -> Result<UrlComponents> {
                Error e{};
                e.message = std::move(msg);
                e.code = Error::Code::InvalidUrl;
                return Result<UrlComponents>::err(std::move(e));
            };

            if (base_url.empty()) {
                return make_err("base_url is empty");
            }
            if (!is_absolute_url_with_protocol(base_url)) {
                return make_err("base_url must start with http:// or https://");
            }

            auto parsed = rest_cpp::parse_url(std::string(base_url));
            if (parsed.has_error())
                return Result<UrlComponents>::err(parsed.error());

            UrlComponents b = std::move(parsed.value());

            // Normalize prefix in b.target
            b.target = trim_trailing_slashes(std::move(b.target));
            if (b.target == "/") b.target.clear();

            // Keep joining simple/fast: reject query in base prefix
            if (b.target.find('?') != std::string::npos) {
                return make_err("base_url must not include query parameters");
            }

            return Result<UrlComponents>::ok(std::move(b));
        }

        inline Result<UrlComponents> resolve_url(std::string_view uri_or_url,
                                                 const UrlComponents* base) {
            auto make_err = [&](std::string msg) -> Result<UrlComponents> {
                Error e{};
                e.message = std::move(msg);
                e.code = Error::Code::InvalidUrl;
                return Result<UrlComponents>::err(std::move(e));
            };

            // Absolute URL => parse (slow path)
            if (is_absolute_url_with_protocol(uri_or_url)) {
                return rest_cpp::parse_url(std::string(uri_or_url));
            }

            // Relative => requires base
            if (base == nullptr || base->host.empty() || base->port.empty()) {
                return make_err("Relative URI provided but base_url is empty");
            }

            // Normalize rel: "" => "/", "health" => "/health"
            std::string_view rel = uri_or_url;
            std::string rel_storage;

            if (rel.empty()) {
                rel = "/";
            } else if (rel.front() != '/') {
                rel_storage.reserve(rel.size() + 1);
                rel_storage.push_back('/');
                rel_storage.append(rel);
                rel = rel_storage;
            }

            // Join prefix(base->target) + rel
            std::string target;
            if (base->target.empty()) {
                target.assign(rel);
            } else {
                target.reserve(base->target.size() + rel.size());
                target.append(base->target);  // prefix has no trailing '/'
                target.append(rel);           // rel begins with '/'
            }

            UrlComponents out;
            out.https = base->https;
            out.host = base->host;
            out.port = base->port;
            out.target = std::move(target);
            return Result<UrlComponents>::ok(std::move(out));
        }

    }  // namespace url_utils

}  // namespace rest_cpp
