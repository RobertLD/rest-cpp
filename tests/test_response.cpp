#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <string>
#include <unordered_map>

#include "gtest/gtest.h"
#include "rest_cpp/response.hpp"

using rest_cpp::copy_response_headers;
using rest_cpp::parse_beast_response;
using rest_cpp::Response;

TEST(ResponseTest, CopyResponseHeaders) {
    namespace http = boost::beast::http;
    http::fields fields;
    fields.set("Content-Type", "application/json");
    fields.set("X-Test", "value");
    fields.set("Content-Type", "text/plain");  // overwrite

    std::unordered_map<std::string, std::string> out;
    copy_response_headers(fields, out);

    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(out["Content-Type"], "text/plain");
    EXPECT_EQ(out["X-Test"], "value");
}

TEST(ResponseTest, ParseBeastResponse) {
    namespace http = boost::beast::http;
    http::response<http::string_body> beast_res;
    beast_res.result(http::status::ok);
    beast_res.set(http::field::server, "test-server");
    beast_res.set(http::field::content_type, "application/json");
    beast_res.body() = "{\"foo\":42}";
    beast_res.prepare_payload();

    Response out = parse_beast_response(std::move(beast_res));
    EXPECT_EQ(out.status_code, 200);
    EXPECT_EQ(out.headers["Server"], "test-server");
    EXPECT_EQ(out.headers["Content-Type"], "application/json");
    EXPECT_EQ(out.body, "{\"foo\":42}");
}