#pragma once
#include <string>

namespace rest_cpp {
    /**
     * @brief Represents an error occurred during an HTTP operation.
     */
    struct Error {
        /** @brief Enumeration of error codes. */
        enum class Code {
            InvalidUrl,        /**< The provided URL is malformed or invalid. */
            ConnectionFailed,  /**< Failed to establish a TCP connection. */
            TlsHandshakeFailed,/**< Failed to perform TLS handshake. */
            Timeout,           /**< The operation timed out. */
            SendFailed,        /**< Failed to send the request. */
            ReceiveFailed,     /**< Failed to receive the response. */
            NetworkError,      /**< General network error. */
            Unknown,           /**< An unknown error occurred. */
        };

        /** @brief The error code. */
        Code code;
        /** @brief A descriptive error message. */
        std::string message;
    };
}  // namespace rest_cpp