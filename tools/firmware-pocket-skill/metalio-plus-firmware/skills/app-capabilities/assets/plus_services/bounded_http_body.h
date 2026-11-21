#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <utility>
namespace plus_services {
// SDK adapter: read(buf,n) -> bytes, 0 EOF, negative error; complete() verifies
// message completion only AFTER readable buffered bytes have been consumed.
// Configure real HTTP/TLS IO deadlines separately; this helper cannot abort a
// blocking reader. cancelled() is tested between bounded read calls.
template<class Read, class Complete, class Cancelled>
bool ReadHttpBody(Read read, Complete complete, Cancelled cancelled,
                  std::size_t limit, std::string* output, std::string* error) {
    auto fail = [&](const char* e) { if (error) *error = e; return false; };
    if (!output || limit == 0) return fail("HTTP_ARGUMENT");
    std::string pending;
    std::array<char, 512> block{};
    for (;;) {
        if (cancelled()) return fail("HTTP_CANCELLED");
        const int count = read(block.data(), block.size());
        if (count < 0) return fail("HTTP_READ_ERROR");
        if (count == 0) {
            if (cancelled()) return fail("HTTP_CANCELLED");
            if (!complete()) return fail("HTTP_TRUNCATED");
            if (pending.empty()) return fail("HTTP_EMPTY");
            *output = std::move(pending);
            if (error) error->clear();
            return true;
        }
        if (static_cast<std::size_t>(count) > block.size() ||
            static_cast<std::size_t>(count) > limit - pending.size()) return fail("HTTP_LIMIT");
        pending.append(block.data(), static_cast<std::size_t>(count));
    }
}
} // namespace plus_services

