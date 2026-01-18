#include <gtest/gtest.h>

#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <string>

#include "rest_cpp/endpoint.hpp"

using namespace rest_cpp;

TEST(ConnectionDetailsTest, ClearResetsFields) {
    Endpoint details{"example.com", "443", true};
    details.clear();
    EXPECT_EQ(details.host, "");
    EXPECT_EQ(details.port, "");
    EXPECT_FALSE(details.https);
}

TEST(ConnectionUtilsTest, IsSameEndpointTrueFalse) {
    EXPECT_TRUE(is_same_endpoint("host", "443", "host", "443"));
    EXPECT_FALSE(is_same_endpoint("host", "443", "other", "443"));
    EXPECT_FALSE(is_same_endpoint("host", "443", "host", "80"));
}

TEST(ConnectionUtilsTest, UpdateEndpointChangesValues) {
    std::string host = "old";
    std::string port = "123";
    update_endpoint(host, port, "new", "456");
    EXPECT_EQ(host, "new");
    EXPECT_EQ(port, "456");
}

TEST(ConnectionUtilsTest, SetSniReturnsTrueOrFalse) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx(boost::asio::ssl::context::sslv23);
    boost::beast::tcp_stream tcp(ioc);
    boost::beast::ssl_stream<boost::beast::tcp_stream> stream(std::move(tcp),
                                                              ctx);
    boost::system::error_code ec;
    // SNI can fail if SSL not initialized, but should set ec
    bool result = set_sni(stream, "example.com", ec);
    EXPECT_TRUE(result || ec);  // Accept either success or error
}

TEST(ConnectionUtilsTest, InitTlsOnSslContextDoesNotThrow) {
    boost::asio::ssl::context ctx(boost::asio::ssl::context::sslv23);
    EXPECT_NO_THROW(init_tls_on_ssl_context(ctx));
}
