
#include <string>

#include "gtest/gtest.h"
#include "rest_cpp/error.hpp"
#include "rest_cpp/result.hpp"

using rest_cpp::Error;
using rest_cpp::Result;

TEST(ResultTest, OkAndHasValue) {
    auto r = Result<int>::ok(42);
    EXPECT_TRUE(r.has_value());
    EXPECT_FALSE(r.has_error());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrorAndHasError) {
    Error err{Error::Code::ConnectionFailed, "fail"};
    auto r = Result<int>::err(err);
    EXPECT_TRUE(r.has_error());
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message, "fail");
    EXPECT_EQ(r.error().code, Error::Code::ConnectionFailed);
}

TEST(ResultTest, ValueOrElseReturnsValue) {
    auto r = Result<std::string>::ok("hello");
    auto val = r.value_or_else([] { return std::string("fallback"); });
    EXPECT_EQ(val, "hello");
}

TEST(ResultTest, ValueOrElseReturnsFallback) {
    Error err{Error::Code::Timeout, "fail"};
    auto r = Result<std::string>::err(err);
    auto val = r.value_or_else([] { return std::string("fallback"); });
    EXPECT_EQ(val, "fallback");
}

TEST(ResultTest, ValueOrReturnsValue) {
    auto r = Result<int>::ok(7);
    EXPECT_EQ(r.value_or(99), 7);
}

TEST(ResultTest, ValueOrReturnsFallback) {
    Error err{Error::Code::ReceiveFailed, "fail"};
    auto r = Result<int>::err(err);
    EXPECT_EQ(r.value_or(99), 99);
}

TEST(ResultTest, ErrorOrReturnsError) {
    Error err{Error::Code::SendFailed, "fail"};
    auto r = Result<int>::err(err);
    Error fallback{Error::Code::Unknown, "fallback"};
    EXPECT_EQ(&r.error_or(fallback), &r.error());
}

TEST(ResultTest, ErrorOrReturnsFallback) {
    auto r = Result<int>::ok(1);
    Error fallback{Error::Code::Unknown, "fallback"};
    EXPECT_EQ(&r.error_or(fallback), &fallback);
}