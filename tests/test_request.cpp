#include <gtest/gtest.h>

#include <boost/beast/http.hpp>

#include "rest_cpp/request.hpp"

using namespace rest_cpp;

TEST(RequestTest, ApplyRequestHeadersSetsFields) {
    boost::beast::http::fields fields;
    std::unordered_map<std::string, std::string> headers = {{"X-Test", "foo"},
                                                            {"X-Bar", "baz"}};
    apply_request_headers(headers, fields);
    EXPECT_EQ(fields["X-Test"], "foo");
    EXPECT_EQ(fields["X-Bar"], "baz");
}

TEST(RequestTest, PrepareBeastRequestBasic) {
    Request req{HttpMethod::Post,
                "http://host/api",
                {{"Content-Type", "application/json"}, {"X-Foo", "bar"}},
                std::string("{\"a\":1}")};
    UrlComponents url;
    url.host = "host";
    url.target = "/api";
    url.https = false;
    url.port = "80";
    std::string user_agent = "test-agent";
    auto beast_req = prepare_beast_request(req, url, user_agent);

    EXPECT_EQ(beast_req.method(), boost::beast::http::verb::post);
    EXPECT_EQ(beast_req.target(), "/api");
    EXPECT_EQ(beast_req[boost::beast::http::field::host], "host");
    EXPECT_EQ(beast_req[boost::beast::http::field::user_agent], "test-agent");
    EXPECT_EQ(beast_req["Content-Type"], "application/json");
    EXPECT_EQ(beast_req["X-Foo"], "bar");
    EXPECT_EQ(beast_req.body(), "{\"a\":1}");
    EXPECT_TRUE(beast_req.keep_alive());
}

TEST(RequestTest, PrepareBeastRequestNoBody) {
    Request req{HttpMethod::Get, "http://host/api", {}, std::nullopt};
    UrlComponents url;
    url.host = "host";
    url.target = "/api";
    url.https = false;
    url.port = "80";
    std::string user_agent = "test-agent";
    auto beast_req = prepare_beast_request(req, url, user_agent);

    EXPECT_EQ(beast_req.method(), boost::beast::http::verb::get);
    EXPECT_EQ(beast_req.body(), "");
}