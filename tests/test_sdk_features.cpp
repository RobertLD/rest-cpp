#include <gtest/gtest.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <future>
#include "rest_cpp/async_client.hpp"
#include "rest_cpp/middleware.hpp"
#include "rest_cpp/serialize_nlohmann.hpp"
#include "rest_cpp/pagination.hpp"
#include "httplib.h"

using namespace rest_cpp;

// DTO for testing
struct Item {
    int id;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Item, id)

// Test Helper for Async
template <class T>
T await_result(boost::asio::io_context& ioc, boost::asio::awaitable<T> aw) {
    auto prom = std::make_shared<std::promise<T>>();
    auto fut = prom->get_future();

    boost::asio::co_spawn(ioc, [aw = std::move(aw), prom]() mutable -> boost::asio::awaitable<void> {
        try {
            T res = co_await std::move(aw);
            prom->set_value(std::move(res));
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
        co_return;
    }, boost::asio::detached);

    std::thread t([&]{ ioc.run(); });
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        ioc.stop();
        t.join();
        throw std::runtime_error("Test timeout");
    }
    auto res = fut.get();
    ioc.stop();
    if (t.joinable()) t.join();
    return res;
}

TEST(SdkFeatures, BearerAuthInterceptor) {
    httplib::Server svr;
    svr.Get("/auth", [](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Authorization") == "Bearer secret-token") {
            res.status = 200;
            res.set_content("ok", "text/plain");
        } else {
            res.status = 401;
        }
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread s_thread([&]{ svr.listen_after_bind(); });

    boost::asio::io_context ioc;
    AsyncRestClientConfiguration cfg;
    cfg.interceptors.push_back(std::make_shared<BearerAuthInterceptor>("secret-token"));

    AsyncRestClient client(ioc.get_executor(), cfg);
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/auth";

    auto res = await_result(ioc, client.get(url));
    EXPECT_EQ(res.value().status_code, 200);

    svr.stop();
    s_thread.join();
}

TEST(SdkFeatures, LinkHeaderPagination) {
    httplib::Server svr;
    int port = svr.bind_to_any_port("127.0.0.1");

    svr.Get("/items", [port](const httplib::Request& req, httplib::Response& res) {
        if (req.target == "/items") {
            std::string next = "http://127.0.0.1:" + std::to_string(port) + "/items?page=2";
            res.set_header("Link", "<" + next + ">; rel=\"next\"");
            res.set_content("[{\"id\":1}, {\"id\":2}]", "application/json");
        } else if (req.target == "/items?page=2") {
            res.set_content("[{\"id\":3}]", "application/json");
        }
    });

    std::thread s_thread([&]{ svr.listen_after_bind(); });

    boost::asio::io_context ioc;
    AsyncRestClient client(ioc.get_executor(), {});
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/items";

    auto result = await_result(ioc, [&]() -> boost::asio::awaitable<int> {
        AsyncPager<Item> pager(client, url);
        int total_id = 0;
        while (auto page = co_await pager.next()) {
            for (const auto& item : page->items) {
                total_id += item.id;
            }
        }
        co_return total_id;
    }());

    EXPECT_EQ(result, 6); // 1+2+3

    svr.stop();
    s_thread.join();
}
