#include <string>

#include "gtest/gtest.h"
#include "rest_cpp/url.hpp"

using rest_cpp::parse_url;
using rest_cpp::ParsedUrl;
using rest_cpp::Result;

TEST(ParseUrlTest, ParsesHttpUrl) {
    auto result = parse_url("http://example.com/foo/bar?baz=1");
    ASSERT_TRUE(result.has_value());
    const ParsedUrl& url = result.value();
    EXPECT_FALSE(url.https);
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "80");
    EXPECT_EQ(url.target, "/foo/bar?baz=1");
}

TEST(ParseUrlTest, ParsesHttpsUrl) {
    auto result = parse_url("https://example.com:8443/path");
    ASSERT_TRUE(result.has_value());
    const ParsedUrl& url = result.value();
    EXPECT_TRUE(url.https);
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "8443");
    EXPECT_EQ(url.target, "/path");
}

TEST(ParseUrlTest, DefaultPort) {
    auto result = parse_url("https://hostonly");
    ASSERT_TRUE(result.has_value());
    const ParsedUrl& url = result.value();
    EXPECT_TRUE(url.https);
    EXPECT_EQ(url.host, "hostonly");
    EXPECT_EQ(url.port, "443");
    EXPECT_EQ(url.target, "/");
}

TEST(ParseUrlTest, MissingScheme) {
    auto result = parse_url("example.com");
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(ParseUrlTest, EmptyHost) {
    auto result = parse_url("http:///foo");
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(ParseUrlTest, EmptyPort) {
    auto result = parse_url("http://host:");
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error().code, rest_cpp::Error::Code::InvalidUrl);
}
