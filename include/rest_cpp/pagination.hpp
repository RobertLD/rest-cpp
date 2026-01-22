#pragma once

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "async_client.hpp"
#include "response.hpp"
#include "result.hpp"
#include "serialize_impl.hpp"

namespace rest_cpp {

    /// @brief Represents a single page of results.
    template <typename T>
    struct Page {
        std::vector<T> items;
        std::optional<std::string> next_url;
    };

    /// @brief Helper to parse RFC 5988 Link headers.
    class LinkHeader {
       public:
        static std::optional<std::string> get_next_url(
            const std::unordered_map<std::string, std::string>& headers) {
            auto it = headers.find("Link");
            if (it == headers.end()) return std::nullopt;

            const std::string& link_header = it->second;
            // Link header can contain multiple links separated by comma
            // Format: <url>; rel="next", <url>; rel="prev"
            size_t start = 0;
            while (start < link_header.size()) {
                size_t end = link_header.find(',', start);
                std::string_view section =
                    std::string_view(link_header)
                        .substr(start, (end == std::string::npos)
                                           ? std::string::npos
                                           : (end - start));

                // Parse <url>
                size_t url_start = section.find('<');
                size_t url_end = section.find('>');
                if (url_start != std::string::npos &&
                    url_end != std::string::npos && url_end > url_start) {
                    std::string_view url =
                        section.substr(url_start + 1, url_end - url_start - 1);

                    // Look for rel="next"
                    if (section.find("rel=\"next\"") != std::string::npos ||
                        section.find("rel=next") != std::string::npos) {
                        return std::string(url);
                    }
                }

                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            return std::nullopt;
        }
    };

    /// @brief Pager for asynchronous result iteration.
    template <typename T>
    class AsyncPager {
       public:
        AsyncPager(AsyncRestClient& client, std::string initial_url)
            : client_(client), next_url_(std::move(initial_url)) {}

        /// @brief Fetch the next page of results.
        /// @return An optional Page<T>. std::nullopt if no more pages or error.
        boost::asio::awaitable<std::optional<Page<T>>> next() {
            if (!next_url_) co_return std::nullopt;

            auto raw_result = co_await client_.get(*next_url_);
            if (raw_result.has_error()) {
                next_url_ = std::nullopt;
                co_return std::nullopt;
            }

            const auto& resp = raw_result.value();
            Page<T> page;

            // Deserialize items
            rest_cpp::deserialize(resp, page.items);

            // Parse next URL from Link header
            page.next_url = LinkHeader::get_next_url(resp.headers);
            next_url_ = page.next_url;

            co_return page;
        }

       private:
        AsyncRestClient& client_;
        std::optional<std::string> next_url_;
    };

}  // namespace rest_cpp
