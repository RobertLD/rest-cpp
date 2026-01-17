#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

#include "error.hpp"

namespace rest_cpp {

    /// @brief Result<T> represents either a successful value of type T or an
    /// Error.
    /// @tparam T The type of the successful value.
    /// @note This is similar to std::expected<T, Error> in C++23.
    /// The goals here are to provide a flexible and type correct
    /// way of interacting with a HTTP Result.
    template <typename T>
    class [[nodiscard]] Result {
       public:
        /// @brief Create a successful Result with the given value.
        /// @tparam Args The types of the arguments to construct T.
        /// @param args The arguments to construct T.
        /// @return A Result containing the constructed T.
        /// @note This overload is only enabled if T is constructible from
        /// Args..., I'd also like to mention I fucking hate templates
        template <typename... Args, typename = std::enable_if_t<
                                        std::is_constructible_v<T, Args&&...>>>
        static Result ok(Args&&... args) {
            // construct the T alternative of the variant directly using these
            // args. Also std::forward is neat
            return Result(std::in_place_type<T>, std::forward<Args>(args)...);
        }

        /// @brief Create an error Result with the given Error.
        /// @param error The Error to store in the Result.
        /// @return A Result containing the given Error.
        static Result err(const Error& error) {
            return Result(std::in_place_type<Error>, error);
        }

        /// @brief Create an error Result with the given Error.
        /// @param error The Error to store in the Result.
        /// @return A Result containing the given Error.
        static Result err(Error&& error) {
            return Result(std::in_place_type<Error>, std::move(error));
        }

        // State Inspection Methods

        /// @brief Allow `if (result) { ... }` to mean "if success".
        explicit operator bool() const noexcept { return has_value(); }

        /// @brief True if this Result currently holds a value of type T.
        /// @return Whether the active variant alternative is T.
        bool has_value() const noexcept {
            return std::holds_alternative<T>(m_state);
        }

        /// @brief True if this Result currently holds an Error.
        /// @return Whether the active variant alternative is Error.
        bool has_error() const noexcept {
            return std::holds_alternative<Error>(m_state);
        }

        // Value Access Methods

        /// @brief Get the stored value (const lvalue overload).
        const T& value() const& {
            const T* p = value_ptr();
            // assert(...) is active in debug builds; it’s removed in release
            // (NDEBUG). We want to be nice to users and not throw exceptions
            // in release builds, so we just assert here.

            assert(p &&
                   "Result::value() called but this Result holds an Error");
            return *p;
        }

        /// @brief Get the stored value (mutable lvalue overload).
        /// @return A mutable reference to the stored T.
        T& value() & {
            T* p = value_ptr();
            assert(p &&
                   "Result::value() called but this Result holds an Error");
            return *p;
        }

        /// @brief Get the stored value (rvalue overload).
        /// @return The stored T, moved out of the Result.
        T&& value() && {
            T* p = value_ptr();
            assert(p &&
                   "Result::value() called but this Result holds an Error");
            return std::move(*p);
        }

        /// @brief Pointer access to the stored value (const).
        [[nodiscard]] const T* value_ptr() const noexcept {
            return std::get_if<T>(&m_state);
        }

        /// @brief Pointer access to the stored value (mutable).

        [[nodiscard]] T* value_ptr() noexcept {
            return std::get_if<T>(&m_state);
        }

        /// @brief Get the stored error (const lvalue overload).
        /// @return Const reference to the stored Error.

        const Error& error() const& {
            const Error* p = error_ptr();
            assert(p && "Result::error() called but this Result holds a value");
            return *p;
        }

        /// @brief Get the stored error (mutable lvalue overload).
        /// @return Mutable reference to the stored Error.
        Error& error() & {
            Error* p = error_ptr();
            assert(p && "Result::error() called but this Result holds a value");
            return *p;
        }

        /// @brief Get the stored error (rvalue overload).
        /// @return Rvalue reference to stored Error (so it can be moved).
        Error&& error() && {
            Error* p = error_ptr();
            assert(p && "Result::error() called but this Result holds a value");
            return std::move(*p);
        }

        /// @brief Pointer access to the stored error (const).
        /// @return Pointer to Error if active, otherwise nullptr.
        [[nodiscard]] const Error* error_ptr() const noexcept {
            return std::get_if<Error>(&m_state);
        }

        /// @brief Pointer access to the stored error (mutable).
        /// @return Pointer to Error if active, otherwise nullptr.
        [[nodiscard]] Error* error_ptr() noexcept {
            return std::get_if<Error>(&m_state);
        }

        // User Convenience Methods
        /// @brief Return the stored value if present, otherwise call a fallback
        /// function.
        ///
        /// This is *lazy* fallback:
        /// - make_fallback() is only invoked when there is no value.
        /// - avoids constructing expensive defaults unnecessarily.
        /// @tparam F Callable type. Must be invocable and return T.
        /// @param make_fallback Callable returning a T used when Result holds
        /// Error.
        /// @return The stored value (copy) or the fallback.
        template <typename F,
                  typename = std::enable_if_t<std::is_invocable_r_v<T, F&&>>>
        T value_or_else(F&& make_fallback) const& {
            return has_value() ? value() : std::forward<F>(make_fallback)();
        }

        /// @brief Rvalue overload of value_or_else to preserve move semantics.

        template <typename F,
                  typename = std::enable_if_t<std::is_invocable_r_v<T, F&&>>>
        T value_or_else(F&& make_fallback) && {
            return has_value() ? std::move(*this).value()
                               : std::forward<F>(make_fallback)();
        }

        /// @brief Eager fallback: return stored value if present, otherwise
        /// return fallback.
        T value_or(T fallback) const& {
            return value_or_else([&] { return std::move(fallback); });
        }

        /// @brief Rvalue overload of value_or preserving move-out behavior.
        T value_or(T fallback) && {
            return std::move(*this).value_or_else(
                [&] { return std::move(fallback); });
        }

        /// @brief Return the stored error if present, otherwise return a
        /// provided fallback reference.
        ///
        /// This is intended for cases like logging paths that want a reference
        /// without branching.
        ///
        /// @param fallback A reference to return if this Result does not hold
        /// an Error.
        /// @return Reference to stored Error if present, otherwise fallback.
        const Error& error_or(const Error& fallback) const noexcept {
            return has_error() ? *error_ptr() : fallback;
        }

       private:
        /// @brief Construct the "value" alternative of the variant in-place.
        /// @tparam Args Argument types forwarded to T's constructor.
        template <typename... Args>
        explicit Result(std::in_place_type_t<T>, Args&&... args)
            : m_state(std::in_place_type<T>, std::forward<Args>(args)...) {}

        /// @brief Construct the "error" alternative of the variant by copy.
        explicit Result(std::in_place_type_t<Error>, const Error& error)
            : m_state(std::in_place_type<Error>, error) {}

        /// @brief Construct the "error" alternative of the variant by move.
        explicit Result(std::in_place_type_t<Error>, Error&& error)
            : m_state(std::in_place_type<Error>, std::move(error)) {}

        /// @brief Storage: exactly one of {T, Error} is active at any time.
        std::variant<T, Error> m_state;
    };

}  // namespace rest_cpp
