#pragma once

#include <simdjson.h>
#include "rest_cpp/serialize_impl.hpp"
#include "rest_cpp/response.hpp"
#include <memory>

namespace rest_cpp {

    class SimdjsonParser {
       public:
        static SimdjsonParser& instance() {
            static SimdjsonParser parser;
            return parser;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded_buffer;

       private:
        SimdjsonParser() = default;
    };

    // User must define how to populate T from simdjson::ondemand::value
    // e.g. void populate(T& out, simdjson::ondemand::value val);

    // We defer the actual parsing logic to a customization point "populate"
    // that users can overload.
    template<typename T>
    void populate(T& out, simdjson::ondemand::value& val);

    template <typename T>
    void deserialize(const Response& response, T& out) {
        auto& p = SimdjsonParser::instance();
        // simdjson requires padding. We might copy here, which is unfortunate but
        // needed if response.body isn't padded.
        p.padded_buffer = simdjson::padded_string(response.body);

        simdjson::ondemand::document_stream docs = p.parser.iterate_many(p.padded_buffer);
        for (auto doc : docs) {
            simdjson::ondemand::value val = doc;
            populate(out, val);
            break; // Just the first one for now
        }
    }

}  // namespace rest_cpp
