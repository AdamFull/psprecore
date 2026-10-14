// Smoke tests for the nx foundation library as built here (libs/foundation): the pieces this project uses.

#include <atomic>
#include <cstdint>

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/serialization/ini.h"
#include "core/foundation/threading/thread_pool.h"
#include "test_main.hpp"

TEST_CASE("foundation: thread pool runs a parallel loop") {
    nx::thread_pool pool(nx::thread_pool::config{.thread_count = 4});
    std::atomic<std::uint64_t> sum{0};
    pool.parallel_for(0, 10000, [&](std::size_t i) { sum.fetch_add(i, std::memory_order_relaxed); });
    CHECK(sum.load() == 10000ull * 9999ull / 2);
}

TEST_CASE("foundation: hash map") {
    nx::flat_hash_map<std::uint32_t, int> map;
    for (std::uint32_t i = 0; i < 1000; ++i) map.insert_or_assign(i * 7919u, static_cast<int>(i));
    CHECK(map.size() == 1000);
    const auto it = map.find(500u * 7919u);
    CHECK(it != map.end() && it->second == 500);
    CHECK(map.find(3u) == map.end());
}

TEST_CASE("foundation: ini round trip") {
    auto parsed = nx::ini::parse("[display]\nscale = 3\n; comment\n[audio]\nvolume = 80\n");
    CHECK(parsed.has_value());
    nx::ini::Document& doc = parsed.value();
    const nx::string* scale = doc.find("display", "scale");
    CHECK(scale && nx::string_view(*scale) == "3");
    CHECK(doc.set("audio", "volume", "55"));
    auto again = nx::ini::parse(doc.serialize());
    CHECK(again.has_value() && nx::string_view(*again.value().find("audio", "volume")) == "55");
}
