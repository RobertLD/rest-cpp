#pragma once

#include "response.hpp"

namespace rest_cpp {

    // Primary template - users should specialize this or overload
    // deserialize(const Response&, T&) via ADL.
    template <typename T>
    void deserialize(const Response& response, T& out);

}  // namespace rest_cpp
