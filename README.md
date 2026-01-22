# rest-cpp

A high-performance, Modern C++ (C++20) REST client library built on top of `Boost.Asio` and `Boost.Beast`.

It is designed for low-latency, high-throughput applications, offering both a simple synchronous API and a powerful asynchronous API using C++20 coroutines.

## Features

- **Protocol Support**: HTTP/1.1, HTTPS (TLS).
- **Asynchronous**: Built-in support for C++20 coroutines (`co_await`).
- **Connection Pooling**:
  - Persistent connection pooling with keep-alive support.
  - Configurable strict limits (per-endpoint and global).
  - Just-in-Time connection creation and reuse strategies.
- **Circuit Breaker**: Automatic failure detection and temporary circuit breaking for unstable endpoints.
- **Thread Safety**: Fully thread-safe asynchronous client and connection pool.
- **Performance**:
  - Waiter scheduling for high-concurrency pool access.
  - Zero-copy semantics where possible.
- **Middleware Support**: Intercept and modify requests (e.g., for authentication) via a flexible interceptor interface.
- **Automatic JSON Serialization**: Deserialize responses into C++ DTOs using `nlohmann/json` or `simdjson`.

## Requirements

- **C++ Standard**: C++20 or later.
- **Boost**: Version 1.80.0 or later (Components: `asio`, `beast`, `system`, `url`).
- **OpenSSL**: For HTTPS support.
- **CMake**: 3.20+ recommended.

## Installation

### Using CMake & vcpkg (Recommended)

This project uses `CMakePresets.json` for easy configuration.

1. **Clone the repository**:
   ```bash
   git clone https://github.com/your-org/rest-cpp.git
   cd rest-cpp
   ```

2. **Configure and Build**:
   ```bash
   # Configure using the default preset
   cmake --preset default

   # Build
   cmake --build --preset default
   ```

### Manually linking

If you have dependencies installed on your system:

```cmake
find_package(Boost 1.80 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
add_subdirectory(rest-cpp)
target_link_libraries(your_app PRIVATE rest_cpp)
```

## Usage

### 1. Synchronous Client (`RestClient`)

Best where blocking is acceptable.

```cpp
#include <rest_cpp/client.hpp>
#include <iostream>

int main() {
    rest_cpp::RestClientConfiguration config;
    config.base_url = "https://api.example.com";
    config.timeout = std::chrono::seconds(5);

    rest_cpp::RestClient client(config);

    // GET request
    auto result = client.get("/v1/status");

    if (result.has_value()) {
        auto& response = result.value();
        std::cout << "Status: " << response.status_code << "\n";
        std::cout << "Body: " << response.body << "\n";
    } else {
        std::cerr << "Error: " << result.error().message << "\n";
    }

    return 0;
}
```

### 2. JSON Serialization & Deserialization

The library supports automatic deserialization of JSON responses into your C++ structs (DTOs).

#### Nlohmann/JSON (Default-ish)
Include `<rest_cpp/serialize_nlohmann.hpp>` and define your DTOs using nlohmann's macros.

```cpp
#include <rest_cpp/client.hpp>
#include <rest_cpp/serialize_nlohmann.hpp>
#include <iostream>

struct User {
    int id;
    std::string name;
};
// Map JSON fields to struct members
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(User, id, name)

int main() {
    rest_cpp::RestClient client({});

    // Use the templated version of get<T>
    auto result = client.get<User>("https://api.example.com/users/1");

    if (result) {
        User user = result.value();
        std::cout << "User: " << user.name << " (ID: " << user.id << ")\n";
    }
}
```

### 3. Middleware / Interceptors

You can apply logic to Every request (like authentication) using Interceptors.

```cpp
#include <rest_cpp/client.hpp>
#include <rest_cpp/middleware.hpp>

int main() {
    rest_cpp::RestClientConfiguration config;

    // Add built-in Bearer Auth interceptor
    config.interceptors.push_back(
        std::make_shared<rest_cpp::BearerAuthInterceptor>("your-token-here")
    );

    // Or use an API Key in query params
    config.interceptors.push_back(
        std::make_shared<rest_cpp::ApiKeyInterceptor>(
            "api_key", "value", rest_cpp::ApiKeyInterceptor::Location::Query)
    );

    rest_cpp::RestClient client(config);
    // All subsequent requests from 'client' will have the token/key applied.
}
```

### 4. Asynchronous Client (`AsyncRestClient`)

 Uses C++20 coroutines.

```cpp
#include <rest_cpp/async_client.hpp>
#include <boost/asio.hpp>
#include <iostream>

using rest_cpp::AsyncRestClient;
using rest_cpp::Result;
using rest_cpp::Response;

boost::asio::awaitable<void> run_client() {
    // 1. Configure the client
    rest_cpp::AsyncRestClientConfiguration config;
    config.base_url = "https://api.example.com";
    config.max_total_connections = 100;
    config.max_connections_per_endpoint = 10;

    // 2. Create the client (shares a ConnectionPool internally)
    // Note: requires an executor (e.g. from the current coroutine frame)
    auto ex = co_await boost::asio::this_coro::executor;
    AsyncRestClient client(ex, config);

    // 3. Send Request
    Result<Response> result = co_await client.get("/v1/data");

    if (result.has_value()) {
        std::cout << "Got data: " << result.value().body.size() << " bytes\n";
    } else {
        std::cout << "Request failed: " << result.error().message << "\n";
    }
}

int main() {
    boost::asio::io_context io;

    boost::asio::co_spawn(io, run_client(), boost::asio::detached);

    io.run();
    return 0;
}
```

## Configuration

### Connection Pool Configuration (`AsyncConnectionPoolConfiguration`)

The `AsyncRestClient` uses a shared `ConnectionPool` designed for high concurrency.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `max_total_connections` | `size_t` | 100 | Soft limit on total open connections in the pool. |
| `max_connections_per_endpoint` | `size_t` | 10 | Limit on concurrent connections to a single `host:port`. |
| `connection_idle_ttl` | `duration` | 30s | Time after which an idle connection is eligible for pruning. |
| `max_connection_reuse_count` | `size_t` | 1000 | Max requests per connection before forcing a rotation (avoids long-lived connection issues). |
| `circuit_breaker_failure_threshold` | `size_t` | 5 | Consecutive failures triggering the circuit breaker. |
| `circuit_breaker_timeout` | `duration` | 30s | How long to stop sending requests to a "broken" endpoint. |

### Client Configuration (`RestClientConfiguration`)
Used for the blocking `RestClient`.

- `base_url`: Optional base URL to prepend to requests.
- `timeout`: Request timeout duration.
- `user_agent`: Custom User-Agent string.

## Error Handling

The library uses a `Result<T, Error>` type (similar to `std::expected` or Rust's `Result`).

- check success with `.has_value()` or `if (result)`.
- access value with `.value()` or `*result`.
- access error with `.error()`.

**Error Codes**:
- `Timeout`: Request exceeded configured timeout.
- `ConnectionFailure`: Network level connection error.
- `CircuitOpen`: Request blocked by circuit breaker explicitly.
- `PoolShutdown`: Client was destroyed while request was in queue.

## Thread Safety

- **`AsyncRestClient`**: Fully thread-safe. You can share a single `AsyncRestClient` instance (or multiple instances sharing the same connection pool) across multiple threads.
- **`RestClient`**: Not thread-safe. Create one instance per thread or lock around usage.

## Performance Tuning

For maximum throughput:
1. Increase `max_total_connections` and `max_connections_per_endpoint`.
2. Ensure you reuse the `AsyncRestClient` instance. The expensive part is the `ConnectionPool` setup; destroying the client destroys the pool.
3. Use a `boost::asio::io_context` with `concurrency_hint` equal to the number of CPU cores.

## License

[MIT License](LICENSE)
