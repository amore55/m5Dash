// Bounded string with no heap allocation.
//
// Used for every settings field, task field and parsed API string. The point is not
// micro-optimisation: it is that the memory cost of a settings struct or a task record is
// known at compile time, so "avoid unbounded dynamic allocations" is enforced by the type
// rather than by discipline.
//
// Truncation is silent by design at the copy site but detectable via truncated(): callers
// that care (e.g. a Telegram token that must be exact) check it.

#pragma once

#include <cstddef>
#include <cstring>

namespace dashboard {

template <std::size_t N>
class FixedString {
    static_assert(N >= 2, "FixedString needs room for at least one character and a NUL");

  public:
    FixedString() { buf_[0] = '\0'; }
    explicit FixedString(const char* s) { assign(s); }

    /// Copies at most N-1 characters. A null pointer clears the string.
    /// Returns false if the source had to be truncated.
    bool assign(const char* s) {
        if (s == nullptr) {
            buf_[0] = '\0';
            return true;
        }
        const std::size_t len = std::strlen(s);
        const std::size_t copy = (len < N - 1) ? len : N - 1;
        std::memcpy(buf_, s, copy);
        buf_[copy] = '\0';
        return copy == len;
    }

    bool assign(const char* s, std::size_t len) {
        if (s == nullptr) {
            buf_[0] = '\0';
            return true;
        }
        const std::size_t copy = (len < N - 1) ? len : N - 1;
        std::memcpy(buf_, s, copy);
        buf_[copy] = '\0';
        return copy == len;
    }

    void clear() { buf_[0] = '\0'; }

    const char* c_str() const { return buf_; }
    char* mutableData() { return buf_; }

    bool empty() const { return buf_[0] == '\0'; }
    std::size_t size() const { return std::strlen(buf_); }
    static constexpr std::size_t capacity() { return N; }

    bool operator==(const FixedString& other) const {
        return std::strcmp(buf_, other.buf_) == 0;
    }
    bool operator!=(const FixedString& other) const { return !(*this == other); }
    bool equals(const char* s) const {
        return s != nullptr && std::strcmp(buf_, s) == 0;
    }

  private:
    char buf_[N];
};

using ShortString = FixedString<32>;
using MediumString = FixedString<64>;
using LongString = FixedString<128>;
using TextString = FixedString<192>;
using UrlString = FixedString<256>;

}  // namespace dashboard
