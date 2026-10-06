#include "core/ParamSet.h"
#include "core/SpscQueue.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <thread>

using namespace ks;

TEST_CASE("SpscQueue basic push/pop and capacity", "[spsc]") {
    SpscQueue<int> q(5); // rounded to 8
    REQUIRE(q.capacity() == 8);
    int v = 0;
    REQUIRE_FALSE(q.pop(v));
    for (int i = 0; i < 8; ++i) REQUIRE(q.push(i));
    REQUIRE_FALSE(q.push(99)); // full
    REQUIRE(q.size() == 8);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue preserves order across threads", "[spsc]") {
    SpscQueue<uint64_t> q(256);
    constexpr uint64_t N = 200000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < N;)
            if (q.push(i)) ++i;
    });
    uint64_t expected = 0, v = 0;
    bool ordered = true;
    while (expected < N) {
        if (q.pop(v)) {
            ordered = ordered && v == expected;
            ++expected;
        }
    }
    producer.join();
    REQUIRE(ordered);
    REQUIRE(q.empty());
}

TEST_CASE("ParamSet defaults, sanitize and lookup", "[params]") {
    static const std::vector<ParamSpec> specs = {
        linearParam("level", "Level", 0.0f, 1.0f, 0.5f),
        intParam("voices", "Voices", 1, 16, 8),
        enumParam("wave", "Wave", {"Saw", "Square", "Tri"}, 1),
        boolParam("on", "On", true),
        logParam("cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, "Hz"),
    };
    ParamSet p(specs);
    REQUIRE(p.size() == 5);
    REQUIRE(p.get(0) == 0.5f);
    REQUIRE(p.get(2) == 1.0f);
    REQUIRE(p.indexOf("cutoff") == 4);
    REQUIRE(p.indexOf("nope") == -1);

    REQUIRE(p.set(0, 2.0f) == 1.0f);      // clamped
    REQUIRE(p.set(1, 3.6f) == 4.0f);      // rounded int
    REQUIRE(p.set(2, 7.0f) == 2.0f);      // enum clamped to last index
    REQUIRE(p.set(3, 0.2f) == 0.0f);      // bool threshold
    REQUIRE(p.set(4, std::numeric_limits<float>::quiet_NaN()) == 1000.0f); // NaN -> default
    REQUIRE(p.set(4, -5.0f) == 20.0f);
    const auto v0 = p.version();
    REQUIRE(p.set("level", 0.25f));
    REQUIRE_FALSE(p.set("missing", 1.0f));
    REQUIRE(p.version() != v0);
    REQUIRE_THAT(p.get(0), Catch::Matchers::WithinAbs(0.25, 1e-6));
    p.resetToDefaults();
    REQUIRE(p.get(1) == 8.0f);
}

TEST_CASE("ParamSpec JSON shape", "[params]") {
    auto s = enumParam("mode", "Mode", {"A", "B"}, 0, "G");
    s.flags = ParamFlags::ReadOnly | ParamFlags::Hidden;
    const auto j = toJson(s);
    REQUIRE(j["scale"] == "enum");
    REQUIRE(j["flags"] == 3);
    REQUIRE(j["choices"].size() == 2);
    REQUIRE(j.contains("skewCentre"));
}
