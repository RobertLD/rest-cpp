#pragma once
#include <string>

namespace rest_cpp {
    struct Error {
        enum class Code {
            InvalidUrl,
            ConnectionFailed,
            TlsHandshakeFailed,
            Timeout,
            SendFailed,
            ReceiveFailed,
            NetworkError,
            Unknown,
        };

        Code code;
        std::string message;
    };
}  // namespace rest_cpp