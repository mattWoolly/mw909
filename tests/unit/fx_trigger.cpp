// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The audible trigger-out click: pulse timing and shape at four rates,
// level, retrigger, determinism, sleep.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/TriggerOut.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> click(double fs, double seconds) {
    TriggerClick t;
    t.prepare(fs);
    t.trigger(Hit::normal());
    std::vector<float> y(static_cast<std::size_t>(fs * seconds));
    t.render(y.data(), static_cast<int>(y.size()));
    return y;
}

}  // namespace

TEST_CASE("trigger click: shape and timing at every rate", "[fx][trigger]") {
    struct Shape {
        double peakV, peakT, troughV, troughT;
    };
    std::vector<Shape> shapes;
    for (double fs : kRates) {
        const auto y = click(fs, 0.1);
        std::size_t ip = 0, it = 0;
        for (std::size_t i = 0; i < y.size(); ++i) {
            if (y[i] > y[ip]) ip = i;
            if (y[i] < y[it]) it = i;
        }
        Shape s{y[ip], static_cast<double>(ip) / fs, y[it], static_cast<double>(it) / fs};
        shapes.push_back(s);
        INFO("fs " << fs << " peak " << s.peakV << " at " << s.peakT << " trough " << s.troughV << " at " << s.troughT);
        // Rising edge: reaches the plateau within ~1 ms (tau 0.1 ms), near 14.1 V x 0.25
        // less the coupling's sag over that time.
        REQUIRE(s.peakT < 0.0012);
        REQUIRE_THAT(s.peakV, WithinRel(14.1 * 0.25 * std::exp(-2.0 * kPi * 10.0 * s.peakT), 0.02));
        // The plateau sags through the 10 Hz coupling: about exp(-2 pi 10 * 14.8 ms) at its end.
        const auto endIdx = static_cast<std::size_t>(0.0147 * fs);
        REQUIRE_THAT(y[endIdx] / s.peakV, WithinAbs(std::exp(-2.0 * kPi * 10.0 * 0.0147), 0.03));
        // Turn-off gives the negative swing, after the pulse's 14.8 + 1.7 ms.
        REQUIRE(s.troughT > 0.0148);
        REQUIRE(s.troughT < 0.025);
        REQUIRE(s.troughV < -0.25 * 14.1 * 0.3);
    }
    for (std::size_t k = 1; k < shapes.size(); ++k) {
        REQUIRE_THAT(shapes[k].peakV, WithinRel(shapes[0].peakV, 0.01));
        REQUIRE_THAT(shapes[k].troughV, WithinRel(shapes[0].troughV, 0.01));
        REQUIRE_THAT(shapes[k].troughT, WithinAbs(shapes[0].troughT, 0.0005));
    }
}

TEST_CASE("trigger click: deterministic, retrigger extends, sleeps", "[fx][trigger]") {
    const double fs = 48000.0;
    REQUIRE(click(fs, 0.05) == click(fs, 0.05));
    TriggerClick t;
    t.prepare(fs);
    REQUIRE_FALSE(t.isActive());
    std::vector<float> z(64);
    t.render(z.data(), 64);
    for (float v : z) REQUIRE(v == 0.0f);
    t.trigger(Hit::normal());
    REQUIRE(t.isActive());
    std::vector<float> y(static_cast<std::size_t>(fs * 0.01));
    t.render(y.data(), static_cast<int>(y.size()));
    t.trigger(Hit::fromCode(63));   // retrigger at 10 ms: pulse stays up, no new edge
    std::vector<float> y2(static_cast<std::size_t>(fs * 0.01));
    t.render(y2.data(), static_cast<int>(y2.size()));
    REQUIRE(y2[0] > 0.0f);
    REQUIRE(std::abs(y2[1] - y2[0]) < 0.01f);    // no step at the retrigger
    REQUIRE(y2.back() > 0.5f * y2[0] * 0.9f);     // still high at 20 ms (would have fallen at 16.5)
    // Sleeps within a second, then renders exact zeros.
    std::vector<float> tail(static_cast<std::size_t>(fs));
    t.render(tail.data(), static_cast<int>(tail.size()));
    REQUIRE_FALSE(t.isActive());
    REQUIRE(std::abs(tail.back()) < 1e-5f);
    // Partition independence.
    TriggerClick a, b;
    a.prepare(fs);
    b.prepare(fs);
    a.trigger(Hit::normal());
    b.trigger(Hit::normal());
    std::vector<float> ya(4800), yb(4800);
    a.render(ya.data(), 4800);
    int off = 0;
    for (int blk : partition(4800, 9u, 300)) {
        b.render(yb.data() + off, blk);
        off += blk;
    }
    REQUIRE(ya == yb);
}
