#include <gtest/gtest.h>
#include <string>
#include <thread>

#include "rest_cpp/client.hpp"
#include "rest_cpp/serialize_simdjson.hpp"
#include "httplib.h"

// Define a DTO
struct Product {
    int id;
    std::string name;
    double price;
};

// Implement populate for simdjson
namespace rest_cpp {
    template<>
    void populate(Product& out, simdjson::ondemand::value& val) {
        simdjson::ondemand::object obj = val.get_object();
        out.id = static_cast<int>(obj["id"].get_int64());
        std::string_view name_sv = obj["name"].get_string();
        out.name = std::string(name_sv);
        out.price = obj["price"].get_double();
    }
}

// Minimal Test Server
struct HttpTestServer {
    explicit HttpTestServer(std::function<void(const httplib::Request&, httplib::Response&)> handler)
        : svr_(), handler_(std::move(handler)) {
        svr_.Get(".*", handler_);
        port_ = svr_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
    }

    ~HttpTestServer() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return static_cast<uint16_t>(port_); }

   private:
    httplib::Server svr_;
    std::function<void(const httplib::Request&, httplib::Response&)> handler_;
    std::thread thread_;
    int port_;
};

static rest_cpp::RestClientConfiguration make_cfg() {
    rest_cpp::RestClientConfiguration cfg{};
    cfg.request_timeout = std::chrono::milliseconds(1000);
    return cfg;
}

static std::string make_url(uint16_t port, std::string path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

TEST(SerializationSimdjson, GetProduct) {
    HttpTestServer srv([](auto const&, auto& res) {
        res.status = 200;
        res.set_content(R"({"id":101,"name":"Widget","price":19.99})", "application/json");
    });

    rest_cpp::RestClient client(make_cfg());
    auto result = client.get<Product>(make_url(srv.port(), "/product"));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().id, 101);
    EXPECT_EQ(result.value().name, "Widget");
    EXPECT_DOUBLE_EQ(result.value().price, 19.99);
}
