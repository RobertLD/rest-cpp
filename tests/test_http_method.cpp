#include <gtest/gtest.h>

#include <boost/beast/http.hpp>

#include "rest_cpp/http_method.hpp"

using namespace rest_cpp;

TEST(HttpMethodTest, ToBoostHttpMethodMapping) {
    EXPECT_EQ(to_boost_http_method(HttpMethod::Get),
              boost::beast::http::verb::get);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Post),
              boost::beast::http::verb::post);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Put),
              boost::beast::http::verb::put);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Patch),
              boost::beast::http::verb::patch);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Delete),
              boost::beast::http::verb::delete_);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Head),
              boost::beast::http::verb::head);
    EXPECT_EQ(to_boost_http_method(HttpMethod::Options),
              boost::beast::http::verb::options);
}

TEST(HttpMethodTest, UnknownMethodReturnsUnknown) {
    // Cast an invalid value to HttpMethod
    EXPECT_EQ(to_boost_http_method(static_cast<HttpMethod>(999)),
              boost::beast::http::verb::unknown);
}
