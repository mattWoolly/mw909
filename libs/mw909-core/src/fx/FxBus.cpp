// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "mw909/fx/FxBus.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace mw909::fx {

void FxRack::prepare(double sampleRate, int maxBlockSize) {
    fs_ = sampleRate;
    maxBlock_ = std::max(1, maxBlockSize);
    distortion_.prepare(sampleRate, maxBlock_);
    for (auto& e : eq_) e.prepare(sampleRate, maxBlock_);
    delay_.prepare(sampleRate, maxBlock_);
    reverb_.prepare(sampleRate, maxBlock_);
    chorus_.prepare(sampleRate, maxBlock_);
    bitcrusher_.prepare(sampleRate, maxBlock_);
    for (auto& d : dynamics_) d.prepare(sampleRate, maxBlock_);
    filter_.prepare(sampleRate, maxBlock_);
    const auto sz = static_cast<std::size_t>(maxBlock_);
    scratchL_.assign(sz, 0.0f);
    scratchR_.assign(sz, 0.0f);
    preL_.assign(sz, 0.0f);
    preR_.assign(sz, 0.0f);
    for (auto& s : slot_) s.fade.setTime(kSlotFadeSeconds, sampleRate);
    toBus2_.setTime(kDefaultSmoothSeconds, sampleRate);
    toBus2_.setEps(1.0e-7);
    toBus2Pre_.setTime(0.010, sampleRate);
    quietMin_ = controlInterval(sampleRate);
    reset();
}

void FxRack::reset() noexcept {
    for (int id = 0; id < kNumFxInstances; ++id) resetInstance(id);
    for (auto& s : slot_) {
        s.placed = false;
        s.armed = false;
        s.fade.snapTo(0.0);
    }
    busQuiet_.fill(kQuietLong);
    // Re-apply the wanted placements at once (no fades after a reset).
    for (auto& s : slot_) {
        if (s.want.on) {
            s.placed = true;
            s.bus = s.want.bus;
            s.order = s.want.order;
            s.fade.snapTo(1.0);
        }
    }
    toBus2_.snap();
    toBus2Pre_.snap();
    rebuild();
    last_.valid = false;
}

void FxRack::setParams(const FxParams& p, double bpm) noexcept {
    bpm = sanitizeBpm(bpm);
    if (same(last_, p, bpm)) return;   // unchanged: nothing to do (cheap per-block calls)
    distortion_.setParams(p.distortion);
    eq_[0].setParams(p.eq[0]);
    eq_[1].setParams(p.eq[1]);
    delay_.setParams(p.delay, bpm);
    reverb_.setParams(p.reverb);
    chorus_.setParams(p.chorus);
    bitcrusher_.setParams(p.bitcrusher);
    dynamics_[0].setParams(p.dynamics[0]);
    dynamics_[1].setParams(p.dynamics[1]);
    filter_.setParams(p.filter, bpm);

    bool changed = false;
    for (int id = 0; id < kNumFxInstances; ++id) {
        auto& s = slot_[static_cast<std::size_t>(id)];
        FxPlacement w = p.place[static_cast<std::size_t>(id)];
        w.bus = w.bus >= kNumFxBuses ? static_cast<std::uint8_t>(kNumFxBuses - 1) : w.bus;
        w.order = w.order > 9 ? static_cast<std::uint8_t>(9) : w.order;
        s.want = w;
        if (!s.placed && w.on) {   // switch on: place now, fade in from silence
            s.placed = true;
            s.bus = w.bus;
            s.order = w.order;
            s.fade.snapTo(0.0);
            s.fade.setTarget(1.0);
            s.armed = true;
            changed = true;
        }
    }
    if (changed) rebuild();

    // Placed instances: stay, adopt a new key in place, or fade out to move/leave.
    changed = false;
    for (int b = 0; b < kNumFxBuses; ++b) {
        const auto bi = static_cast<std::size_t>(b);
        // The bus's chain as it runs now, and as it would run if every instance
        // staying on the bus took its wanted key at once (the others, fading
        // out to leave, keep theirs).
        std::array<std::uint8_t, kNumFxInstances> now{}, wanted{};
        const int cn = count_[bi];
        int cw = 0;
        auto stays = [&](int id) {
            const auto& s = slot_[static_cast<std::size_t>(id)];
            return s.placed && s.bus == b && s.want.on && s.want.bus == b;
        };
        auto keyOf = [&](int id) {
            const auto& s = slot_[static_cast<std::size_t>(id)];
            return stays(id) ? s.want.order : s.order;
        };
        for (int k = 0; k < cn; ++k) now[static_cast<std::size_t>(k)] = chain_[bi][static_cast<std::size_t>(k)];
        for (int id = 0; id < kNumFxInstances; ++id) {   // stable insertion by (key, id)
            const auto& s = slot_[static_cast<std::size_t>(id)];
            if (!s.placed || s.bus != b) continue;
            const auto key = keyOf(id);
            int pos = cw++;
            while (pos > 0 && keyOf(wanted[static_cast<std::size_t>(pos - 1)]) > key) {
                wanted[static_cast<std::size_t>(pos)] = wanted[static_cast<std::size_t>(pos - 1)];
                --pos;
            }
            wanted[static_cast<std::size_t>(pos)] = static_cast<std::uint8_t>(id);
        }
        const bool sameOrder = cn == cw && now == wanted;
        for (int id = 0; id < kNumFxInstances; ++id) {
            auto& s = slot_[static_cast<std::size_t>(id)];
            if (!s.placed || s.bus != b) continue;
            if (!stays(id)) {
                s.fade.setTarget(0.0);   // off or to the other bus
            } else if (s.want.order == s.order) {
                s.fade.setTarget(1.0);
            } else if (sameOrder) {   // new key, same chain order: nothing to hear
                s.order = s.want.order;
                s.fade.setTarget(1.0);
                changed = true;
            } else {
                s.fade.setTarget(0.0);   // reorder: fade out, move (state kept), fade in
            }
        }
    }
    if (changed) rebuild();

    const auto& b1 = p.bus[0];
    toBus2_.setTarget(sanitize(b1.toBus2, 0.0f, 0.0f, 1.0f));
    toBus2Pre_.setTarget(b1.toBus2Pre ? 1.0 : 0.0);
}

void FxRack::noteTrigger(VoiceId v) noexcept {
    const int voice = static_cast<int>(v);
    for (int i = 0; i < 2; ++i) {
        const auto& s = slot_[static_cast<std::size_t>(static_cast<int>(FxId::DynamicsA) + i)];
        if (s.placed && s.want.on) dynamics_[static_cast<std::size_t>(i)].noteTrigger(voice);
    }
    const auto& f = slot_[static_cast<std::size_t>(FxId::Filter)];
    if (f.placed && f.want.on) filter_.noteTrigger(voice);
}

void FxRack::rebuild() noexcept {
    count_.fill(0);
    // Stable insertion by (order, id): ids are visited in ascending order.
    for (int id = 0; id < kNumFxInstances; ++id) {
        const auto& s = slot_[static_cast<std::size_t>(id)];
        if (!s.placed) continue;
        auto& ch = chain_[s.bus];
        int& c = count_[s.bus];
        int pos = c;
        while (pos > 0 && slot_[ch[static_cast<std::size_t>(pos - 1)]].order > s.order) {
            ch[static_cast<std::size_t>(pos)] = ch[static_cast<std::size_t>(pos - 1)];
            --pos;
        }
        ch[static_cast<std::size_t>(pos)] = static_cast<std::uint8_t>(id);
        ++c;
    }
}

// Slots whose fade-out finished (at this very sample): move them or take them out.
void FxRack::settleSlots() noexcept {
    bool changed = false;
    for (int id = 0; id < kNumFxInstances; ++id) {
        auto& s = slot_[static_cast<std::size_t>(id)];
        if (!s.placed || !s.fade.settled() || s.fade.target() != 0.0) continue;
        if (!s.want.on) {   // switched off: silence it and take it out
            resetInstance(id);
            s.placed = false;
        } else {
            if (s.want.bus != s.bus) resetInstance(id);   // to the other bus: start from silence
            s.bus = s.want.bus;                            // within the bus: keep the state (tails ring on)
            s.order = s.want.order;
            s.fade.setTarget(1.0);
        }
        changed = true;
    }
    if (changed) rebuild();
}

// Samples since bus b's input was last above the floor (backward scan: O(1)
// while audio flows), exact whatever the chunking.
long FxRack::quietAfter(int b, const float* l, const float* r, int n) const noexcept {
    for (int k = n - 1; k >= 0; --k)
        if (std::abs(l[k]) > kSleepFloor || std::abs(r[k]) > kSleepFloor) return n - 1 - k;
    const long q = busQuiet_[static_cast<std::size_t>(b)];
    return q + n < kQuietLong ? q + n : kQuietLong;
}

// Samples until the first fade-out ends (0: none running).
int FxRack::samplesToNextSettle() const noexcept {
    int m = 0;
    for (const auto& s : slot_) {
        if (!s.placed || s.fade.target() != 0.0 || s.fade.settled()) continue;
        const int r = s.fade.remaining();
        m = m == 0 || r < m ? r : m;
    }
    return m;
}

// An asleep instance that would leave a silent input untouched (bypassed)
// rather than writing zeros.
bool FxRack::instancePassThrough(int id) const noexcept {
    switch (static_cast<FxId>(id)) {
        case FxId::EqA: return eq_[0].passThrough();
        case FxId::EqB: return eq_[1].passThrough();
        case FxId::Filter: return filter_.passThrough();
        default: return false;
    }
}

int FxRack::chain(int b, std::array<std::uint8_t, kNumFxInstances>& ids) const noexcept {
    const auto bi = static_cast<std::size_t>(b & 1);
    ids = chain_[bi];
    return count_[bi];
}

void FxRack::resetInstance(int id) noexcept {
    switch (static_cast<FxId>(id)) {
        case FxId::Distortion: distortion_.reset(); break;
        case FxId::EqA: eq_[0].reset(); break;
        case FxId::EqB: eq_[1].reset(); break;
        case FxId::Delay: delay_.reset(); break;
        case FxId::Reverb: reverb_.reset(); break;
        case FxId::Chorus: chorus_.reset(); break;
        case FxId::Bitcrusher: bitcrusher_.reset(); break;
        case FxId::DynamicsA: dynamics_[0].reset(); break;
        case FxId::DynamicsB: dynamics_[1].reset(); break;
        case FxId::Filter: filter_.reset(); break;
        default: break;
    }
}

bool FxRack::instanceActive(int id) const noexcept {
    switch (static_cast<FxId>(id)) {
        case FxId::Distortion: return distortion_.isActive();
        case FxId::EqA: return eq_[0].isActive();
        case FxId::EqB: return eq_[1].isActive();
        case FxId::Delay: return delay_.isActive();
        case FxId::Reverb: return reverb_.isActive();
        case FxId::Chorus: return chorus_.isActive();
        case FxId::Bitcrusher: return bitcrusher_.isActive();
        case FxId::DynamicsA: return dynamics_[0].isActive();
        case FxId::DynamicsB: return dynamics_[1].isActive();
        case FxId::Filter: return filter_.isActive();
        default: return false;
    }
}

double FxRack::instanceTail(int id) const noexcept {
    switch (static_cast<FxId>(id)) {
        case FxId::Distortion: return distortion_.tailSeconds();
        case FxId::EqA: return eq_[0].tailSeconds();
        case FxId::EqB: return eq_[1].tailSeconds();
        case FxId::Delay: return delay_.tailSeconds();
        case FxId::Reverb: return reverb_.tailSeconds();
        case FxId::Chorus: return chorus_.tailSeconds();
        case FxId::Bitcrusher: return bitcrusher_.tailSeconds();
        case FxId::DynamicsA: return dynamics_[0].tailSeconds();
        case FxId::DynamicsB: return dynamics_[1].tailSeconds();
        case FxId::Filter: return filter_.tailSeconds();
        default: return 0.0;
    }
}

void FxRack::runInstance(int id, float* l, float* r, int n, const std::array<const float*, kNumVoices>& key,
                         int offset) noexcept {
    auto keyOf = [&](const Dynamics& d) -> const float* {
        const int v = d.sidechainVoice();
        if (v < 0) return nullptr;
        const float* k = key[static_cast<std::size_t>(v)];
        return k != nullptr ? k + offset : nullptr;
    };
    switch (static_cast<FxId>(id)) {
        case FxId::Distortion: distortion_.process(l, r, n); break;
        case FxId::EqA: eq_[0].process(l, r, n); break;
        case FxId::EqB: eq_[1].process(l, r, n); break;
        case FxId::Delay: delay_.process(l, r, n); break;
        case FxId::Reverb: reverb_.process(l, r, n); break;
        case FxId::Chorus: chorus_.process(l, r, n); break;
        case FxId::Bitcrusher: bitcrusher_.process(l, r, n); break;
        case FxId::DynamicsA: dynamics_[0].process(l, r, n, keyOf(dynamics_[0])); break;
        case FxId::DynamicsB: dynamics_[1].process(l, r, n, keyOf(dynamics_[1])); break;
        case FxId::Filter: filter_.process(l, r, n); break;
        default: break;
    }
}

void FxRack::processBus(int b, float* l, float* r, int n, const std::array<const float*, kNumVoices>& key,
                        int offset, bool silent) noexcept {
    const auto bi = static_cast<std::size_t>(b);
    const int cnt = count_[bi];
    if (cnt == 0) return;   // no effects: the return is the input
    // Silent input, every instance asleep, no fade running and no sidechain
    // key that would wake a compressor: running the chain would only write
    // what this writes (zeros, or the input itself if every instance is
    // bypassed), so skip it.
    if (silent) {
        bool quiet = true, zeros = false;
        for (int k = 0; k < cnt && quiet; ++k) {
            const int id = chain_[bi][static_cast<std::size_t>(k)];
            const auto& s = slot_[static_cast<std::size_t>(id)];
            if (!s.fade.settled() || instanceActive(id)) {
                quiet = false;
                break;
            }
            if (s.fade.value() == 0.0) continue;   // out of the path
            if (id == static_cast<int>(FxId::DynamicsA) || id == static_cast<int>(FxId::DynamicsB)) {
                const int v = dynamics_[static_cast<std::size_t>(id - static_cast<int>(FxId::DynamicsA))].sidechainVoice();
                const float* kp = v >= 0 ? key[static_cast<std::size_t>(v)] : nullptr;
                if (kp != nullptr && firstAbove(kp + offset, nullptr, n, kSleepFloor) < n) quiet = false;
            }
            if (!instancePassThrough(id)) zeros = true;
        }
        if (quiet) {
            if (zeros) {
                zeroBuf(l, n);
                zeroBuf(r, n);
            }
            return;
        }
    }
    for (int k = 0; k < cnt; ++k) {
        const int id = chain_[bi][static_cast<std::size_t>(k)];
        auto& s = slot_[static_cast<std::size_t>(id)];
        if (s.armed) {   // first sample after a switch-on: silence in, nothing to crossfade
            s.armed = false;
            if (s.fade.target() > 0.5 && !s.fade.settled() && busQuiet_[bi] >= quietMin_ &&
                std::abs(l[0]) <= kSleepFloor && std::abs(r[0]) <= kSleepFloor && !instanceActive(id))
                s.fade.snap();
        }
        if (s.fade.settled() && s.fade.value() == 0.0) continue;
        if (s.fade.settled()) {
            runInstance(id, l, r, n, key, offset);
            continue;
        }
        float* sl = scratchL_.data();
        float* sr = scratchR_.data();
        std::memcpy(sl, l, sizeof(float) * static_cast<std::size_t>(n));
        std::memcpy(sr, r, sizeof(float) * static_cast<std::size_t>(n));
        runInstance(id, sl, sr, n, key, offset);
        for (int i = 0; i < n; ++i) {
            const auto e = static_cast<float>(s.fade.tick());
            if (e >= 1.0f) {   // exactly the in-place result once the fade is done
                l[i] = sl[i];
                r[i] = sr[i];
            } else {
                l[i] += e * (sl[i] - l[i]);
                r[i] += e * (sr[i] - r[i]);
            }
        }
    }
}

void FxRack::process(float* const (&bus)[kNumFxBuses][2], const std::array<const float*, kNumVoices>& key,
                     int n) noexcept {
    if (maxBlock_ <= 0) return;   // not prepared: leave the buses as they are
    // One bad sample upstream must not poison the effects' state for good
    // (NaN/Inf -> 0). Skipped when no effect is placed (the buses pass as they are).
    if (count_[0] + count_[1] > 0)
        for (int b = 0; b < kNumFxBuses; ++b)
            for (int c = 0; c < 2; ++c) {
                float* x = bus[b][c];
                for (int i = 0; i < n; ++i) {   // branch-free: vectorises
                    const float v = x[i];
                    x[i] = std::abs(v) <= FLT_MAX ? v : 0.0f;
                }
            }
    int offset = 0;
    while (offset < n) {
        settleSlots();   // a fade-out that ended exactly at the previous call's end
        int m = std::min(n - offset, maxBlock_);
        const int ev = samplesToNextSettle();   // cut the work where a fade-out ends
        if (ev > 0 && ev < m) m = ev;
        float* l1 = bus[0][0] + offset;
        float* r1 = bus[0][1] + offset;
        float* l2 = bus[1][0] + offset;
        float* r2 = bus[1][1] + offset;
        const bool send = toBus2_.value() > 0.0 || toBus2_.target() > 0.0;
        const bool needPre = send && (toBus2Pre_.value() > 0.0 || toBus2Pre_.target() > 0.0);
        if (needPre) {
            std::memcpy(preL_.data(), l1, sizeof(float) * static_cast<std::size_t>(m));
            std::memcpy(preR_.data(), r1, sizeof(float) * static_cast<std::size_t>(m));
        }
        const long q1 = quietAfter(0, l1, r1, m);
        processBus(0, l1, r1, m, key, offset, q1 >= m);   // no sample above the floor in this chunk
        busQuiet_[0] = q1;
        if (send) {
            for (int i = 0; i < m; ++i) {
                const double a = toBus2_.tick();
                // The send just closed: the pre/post switch jumps to its target
                // at this sample (as it does in every block without a send).
                if (a == 0.0 && toBus2_.target() == 0.0) toBus2Pre_.snap();
                const double w = toBus2Pre_.tick();
                const double sl = needPre ? l1[i] + w * (preL_[static_cast<std::size_t>(i)] - l1[i]) : l1[i];
                const double sr = needPre ? r1[i] + w * (preR_[static_cast<std::size_t>(i)] - r1[i]) : r1[i];
                l2[i] = static_cast<float>(l2[i] + a * sl);
                r2[i] = static_cast<float>(r2[i] + a * sr);
            }
        } else {
            toBus2Pre_.snap();   // no send: nothing to crossfade
        }
        const long q2 = quietAfter(1, l2, r2, m);
        processBus(1, l2, r2, m, key, offset, q2 >= m);
        busQuiet_[1] = q2;
        settleSlots();
        offset += m;
    }
}

bool FxRack::busActive(int b) const noexcept {
    const auto bi = static_cast<std::size_t>(b & 1);
    for (int k = 0; k < count_[bi]; ++k) {
        const int id = chain_[bi][static_cast<std::size_t>(k)];
        const auto& s = slot_[static_cast<std::size_t>(id)];
        if (!s.fade.settled()) return true;
        if (s.fade.value() > 0.0 && instanceActive(id)) return true;
    }
    if (bi == 1 && (toBus2_.value() > 0.0 || toBus2_.target() > 0.0)) return busActive(0);
    return false;
}

double FxRack::tailSeconds(int b) const noexcept {
    const auto bi = static_cast<std::size_t>(b & 1);
    double t = 0.0;
    for (int k = 0; k < count_[bi]; ++k) t = std::max(t, instanceTail(chain_[bi][static_cast<std::size_t>(k)]));
    if (bi == 1 && toBus2_.target() > 0.0) t += tailSeconds(0);
    return t;
}

}  // namespace mw909::fx
