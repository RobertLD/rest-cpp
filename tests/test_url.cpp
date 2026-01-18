#include <string>

#include "gtest/gtest.h"
#include "rest_cpp/url.hpp"

using rest_cpp::parse_url;
using rest_cpp::UrlComponents;
using namespace rest_cpp::url_utils;

TEST(ParseUrlTest, ParsesHttpUrl) {
    auto result = parse_url("http://example.com/foo/bar?baz=1");
    ASSERT_TRUE(result.has_value());
    const UrlComponents& url = result.value();
    EXPECT_FALSE(url.https);
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "80");
    EXPECT_EQ(url.target, "/foo/bar?baz=1");
}

TEST(ParseUrlTest, ParsesHttpsUrl) {
    auto result = parse_url("https://example.com:8443/path");
    ASSERT_TRUE(result.has_value());
    const UrlComponents& url = result.value();
    EXPECT_TRUE(url.https);
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "8443");
    EXPECT_EQ(url.target, "/path");
}

TEST(ParseUrlTest, DefaultPort) {
    auto result = parse_url("https://hostonly");
    ASSERT_TRUE(result.has_value());
    const UrlComponents& url = result.value();
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

TEST(UrlUtilsTest, IsAbsoluteUrlWithProtocol) {
    EXPECT_TRUE(is_absolute_url_with_protocol("http://example.com"));
    EXPECT_TRUE(is_absolute_url_with_protocol("https://example.com"));
    EXPECT_FALSE(is_absolute_url_with_protocol("ftp://example.com"));
    EXPECT_FALSE(is_absolute_url_with_protocol("example.com"));
}

TEST(UrlUtilsTest, TrimTrailingSlashes) {
    EXPECT_EQ(trim_trailing_slashes("/foo/bar/"), "/foo/bar");
    EXPECT_EQ(trim_trailing_slashes("/foo/bar"), "/foo/bar");
    EXPECT_EQ(trim_trailing_slashes("/"), "");
    EXPECT_EQ(trim_trailing_slashes(""), "");
}

TEST(UrlUtilsTest, CombineBaseAndUri) {
    auto r1 = combine_base_and_uri("http://host", "api");
    EXPECT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value(), "http://host/api");

    auto r2 = combine_base_and_uri("http://host/", "/api");
    EXPECT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value(), "http://host/api");

    auto r3 = combine_base_and_uri("http://host", "");
    EXPECT_TRUE(r3.has_value());
    EXPECT_EQ(r3.value(), "http://host/");

    auto r4 = combine_base_and_uri("http://host", "http://other/api");
    EXPECT_TRUE(r4.has_value());
    EXPECT_EQ(r4.value(), "http://other/api");

    auto r5 = combine_base_and_uri("", "api");
    EXPECT_TRUE(r5.has_error());
    EXPECT_EQ(r5.error().code, rest_cpp::Error::Code::InvalidUrl);

    auto r6 = combine_base_and_uri("host", "api");
    EXPECT_TRUE(r6.has_error());
    EXPECT_EQ(r6.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(UrlUtilsTest, ParseBaseUrl) {
    auto r1 = parse_base_url("http://host/api");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().target, "/api");

    auto r2 = parse_base_url("http://host/");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().target, "");

    auto r3 = parse_base_url("http://host/api?x=1");
    EXPECT_TRUE(r3.has_error());
    EXPECT_EQ(r3.error().code, rest_cpp::Error::Code::InvalidUrl);

    auto r4 = parse_base_url("");
    EXPECT_TRUE(r4.has_error());
    EXPECT_EQ(r4.error().code, rest_cpp::Error::Code::InvalidUrl);
}

TEST(UrlUtilsTest, ResolveUrlAbsoluteAndRelative) {
    auto base = parse_base_url("http://host/api").value();
    auto abs = resolve_url("http://other/foo", &base);
    ASSERT_TRUE(abs.has_value());
    EXPECT_EQ(abs.value().host, "other");
    EXPECT_EQ(abs.value().target, "/foo");

    auto rel1 = resolve_url("health", &base);
    ASSERT_TRUE(rel1.has_value());
    EXPECT_EQ(rel1.value().host, "host");
    EXPECT_EQ(rel1.value().target, "/api/health");

    auto rel2 = resolve_url("/bar", &base);
    ASSERT_TRUE(rel2.has_value());
    EXPECT_EQ(rel2.value().target, "/api/bar");

    auto rel3 = resolve_url("", &base);
    ASSERT_TRUE(rel3.has_value());
    EXPECT_EQ(rel3.value().target, "/api/");

    auto rel4 = resolve_url("foo", nullptr);
    EXPECT_TRUE(rel4.has_error());
    EXPECT_EQ(rel4.error().code, rest_cpp::Error::Code::InvalidUrl);
}
