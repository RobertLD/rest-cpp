#pragma once

#include <nlohmann/json.hpp>
#include "rest_cpp/serialize_impl.hpp"
#include "rest_cpp/response.hpp"

namespace rest_cpp {

    // Overload for types adaptable to nlohmann::json
    template <typename T>
    void deserialize(const Response& response, T& out) {
        auto j = nlohmann::json::parse(response.body);
        j.get_to(out);
    }

}  // namespace rest_cpp
