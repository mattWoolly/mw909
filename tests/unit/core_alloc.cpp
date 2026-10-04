// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Zero allocations in Machine::process (architecture.md §1, §7): operator
// new/delete AND malloc/calloc/realloc/free (and the aligned variants) are
// interposed for this executable and counted while process() runs. Covered:
// block sizes 1..8192, sample rates 44.1..192 kHz, every quality, unit-seed /
// variance / revision changes (the voices are re-prepared on the audio
// thread), routing and FX changes, MIDI in and out, tap write, every command,
// bank and user-ROM handoffs.
//
// Under AddressSanitizer / ThreadSanitizer only operator new is counted (the
// sanitizer owns malloc).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>

#include "CoreTestUtil.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MW909_COUNT_MALLOC 0
#else
#define MW909_COUNT_MALLOC 1
#endif

namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gNews{0}, gMallocs{0}, gFrees{0};
}  // namespace

#if MW909_COUNT_MALLOC
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

void* malloc(std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    return __libc_malloc(n);
}
void* calloc(std::size_t a, std::size_t b) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    return __libc_calloc(a, b);
}
void* realloc(void* p, std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    return __libc_realloc(p, n);
}
void* memalign(std::size_t align, std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    return __libc_memalign(align, n);
}
void* aligned_alloc(std::size_t align, std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    return __libc_memalign(align, n);
}
int posix_memalign(void** out, std::size_t align, std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gMallocs;
    void* p = __libc_memalign(align, n);
    if (p == nullptr) return 12;   // ENOMEM
    *out = p;
    return 0;
}
void free(void* p) {
    if (p != nullptr && gCounting.load(std::memory_order_relaxed)) ++gFrees;
    __libc_free(p);
}
}
#endif

void* operator new(std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gNews;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) ++gNews;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (p != nullptr && gCounting.load(std::memory_order_relaxed)) ++gFrees;
    std::free(p);
}
void operator delete[](void* p) noexcept {
    if (p != nullptr && gCounting.load(std::memory_order_relaxed)) ++gFrees;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete[](p); }

using namespace mw909;
using namespace coretest;

namespace {

struct Counted {
    long news = 0, mallocs = 0, frees = 0;
};

// process() with the counters armed.
Counted countedProcess(Machine& m, const Machine::ProcessArgs& a, const MachineParams& p) {
    const long n0 = gNews.load(), m0 = gMallocs.load(), f0 = gFrees.load();
    gCounting.store(true);
    m.process(a, p);
    gCounting.store(false);
    return {gNews.load() - n0, gMallocs.load() - m0, gFrees.load() - f0};
}

MachineParams fxParams() {
    MachineParams p = defaultMachineParams();
    auto& e = p.engine;
    e.channel[SD].sendBus = 2;
    e.channel[SD].send = 0.5f;
    e.channel[SD].spread = 0.6f;
    e.channel[CP].route = ChannelRoute::FxBus1;
    e.channel[CH].output = 2;
    e.channel[BD].drive = 0.5f;
    e.strip[BD].compOn = true;
    e.strip[BD].eqOn = true;
    e.strip[BD].band[0] = {true, 100.0f, 1.0f, 6.0f};
    for (int i = 0; i < kNumFxInstances; ++i)
        e.fx.place[static_cast<std::size_t>(i)] = {true, static_cast<std::uint8_t>(i % 2), static_cast<std::uint8_t>(i)};
    e.fx.dynamics[1].mode = DynamicsMode::Vca;
    e.fx.dynamics[1].triggers[0] = BD;
    e.fx.dynamics[0].sidechain = BD;
    e.fx.filter.source = ModSource::Envelope;
    e.fx.filter.envTriggers[0] = SD;
    e.fx.bus[0].toBus2 = 0.5f;
    e.trig[0].on = true;
    e.trig[1] = {true, BD, ChannelRoute::FxBus2, 0, 0.0f, 0.5f};
    e.master.compOn = true;
    e.master.stage = OutputStage::Original;
    p.seq.record = true;
    p.midi.midiOut = true;
    p.midi.bassChannel = 2;
    return p;
}

}  // namespace

TEST_CASE("Machine::process never allocates or frees", "[core][alloc]") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        INFO("rate " << fs);
        auto m = std::make_unique<Machine>();
        m->prepare(fs, 8192);
        postPattern(*m, 0, densePattern());
        postPattern(*m, 1, patternWith({BD, OH}, {0, 3, 6}));

        MachineParams p = fxParams();
        p.seq.mode = seq::Mode::HostSync;
        Buses buses(8192);
        MidiOutBuffer midiOut;
        std::vector<MidiMessage> midiIn;
        std::mt19937 rng(3);

        // Pre-allocated handoffs, posted from "the UI" between blocks.
        std::vector<std::unique_ptr<BankHandoff>> banks;
        std::vector<std::unique_ptr<RomHandoff>> roms;
        for (int i = 0; i < 4; ++i) {
            auto b = std::make_unique<BankHandoff>();
            b->bank.patterns[0] = densePattern();
            b->generation = static_cast<std::uint32_t>(i + 1);
            banks.push_back(std::move(b));
            auto r = std::make_unique<RomHandoff>();
            r->slot = static_cast<RomSlot>(i % kNumRomSlots);
            r->clear = i == 3;
            for (auto& w : r->image.data) w = static_cast<std::uint8_t>(rng() % 64);
            roms.push_back(std::move(r));
        }

        double ppq = 0.0;
        const int sizes[] = {1, 2, 3, 17, 64, 128, 256, 511, 1024, 2048, 2049, 4096, 8191, 8192};
        Counted total;
        for (int round = 0; round < 60; ++round) {
            const int n = sizes[static_cast<std::size_t>(round) % std::size(sizes)];
            // Change something every round.
            switch (round % 12) {
                case 0: p.quality = static_cast<Quality>((round / 12) % 3); break;
                case 1: p.unitSeed = static_cast<std::uint32_t>(round); p.variance = 0.3f; break;
                case 2: p.revision = round % 24 == 2 ? Revision::Early : Revision::Late; break;
                case 3: p.engine.channel[RS].output = static_cast<std::uint8_t>(rng() % kNumBuses); break;
                case 4: p.engine.fx.place[static_cast<std::size_t>(FxId::Reverb)].bus ^= 1U; break;
                case 5: p.engine.strip[SD].compOn = !p.engine.strip[SD].compOn; break;
                case 6: p.seq.mode = p.seq.mode == seq::Mode::HostSync ? seq::Mode::Internal : seq::Mode::HostSync; p.seq.run = true; break;
                case 7: p.engine.channel[SD].spread = static_cast<float>(rng() % 100) / 100.0f; break;
                case 8: p.engine.hatChoke = !p.engine.hatChoke; break;
                case 9: p.seq.mode = seq::Mode::HostNote; break;
                default: break;
            }
            // Commands and handoffs.
            Command c;
            switch (round % 7) {
                case 0: c.type = Command::Type::Audition; c.index = round % kNumVoices; c.value = 3.0f; break;
                case 1: c.type = Command::Type::SelectPattern; c.index = round % 2; break;
                case 2: c.type = Command::Type::Fill; break;
                case 3: c.type = Command::Type::SetPattern; c.index = 1; c.pattern = densePattern(); c.generation = 99; break;
                case 4: c.type = Command::Type::SetChain; c.chain.length = 2; c.chain.entries[1] = 1; break;
                case 5: c.type = Command::Type::Panic; break;
                default: c.type = Command::Type::SetSongTrack; c.index = 1; c.songTrack.length = 3; break;
            }
            m->post(c);
            if (round % 5 == 0 && !banks.empty()) {
                BankHandoff* old = m->postBank(banks.back().release());
                banks.pop_back();
                delete old;
            }
            if (round % 4 == 1 && !roms.empty()) {
                RomHandoff* old = m->postUserRom(roms.back().release());
                roms.pop_back();
                delete old;
            }
            // MIDI in for this block.
            midiIn.clear();
            for (int k = 0; k < 6; ++k) {
                const int note = (k % 2 == 0) ? kVoiceNote[static_cast<std::size_t>(rng() % kNumVoices)]
                                              : static_cast<int>(24 + rng() % 96);
                midiIn.push_back(noteOn(static_cast<int>(rng() % static_cast<unsigned>(n)), note,
                                        static_cast<int>(1 + rng() % 127), 1 + static_cast<int>(rng() % 3)));
            }
            std::sort(midiIn.begin(), midiIn.end(), [](const MidiMessage& a, const MidiMessage& b) { return a.offset < b.offset; });

            Machine::ProcessArgs a;
            a.transport.hostValid = true;
            a.transport.playing = true;
            a.transport.bpm = 128.0;
            a.transport.ppq = ppq;
            a.midiIn = midiIn.data();
            a.numMidiIn = static_cast<int>(midiIn.size());
            a.midiOut = &midiOut;
            a.out = buses.view();
            a.numSamples = n;
            const Counted got = countedProcess(*m, a, p);
            total.news += got.news;
            total.mallocs += got.mallocs;
            total.frees += got.frees;
            ppq += n * 128.0 / (60.0 * fs);

            // The UI drains what the audio thread retired (outside the count).
            Notice note;
            while (m->poll(note)) {}
            while (BankHandoff* h = m->takeRetiredBank()) delete h;
            while (RomHandoff* h = m->takeRetiredRom()) delete h;
        }
        REQUIRE(total.news == 0);
        REQUIRE(total.mallocs == 0);
        REQUIRE(total.frees == 0);
    }
}

TEST_CASE("The counters do count (self-test)", "[core][alloc]") {
    const long n0 = gNews.load(), m0 = gMallocs.load();
    gCounting.store(true);
    auto* p = new int(3);
    void* q = std::malloc(16);
    gCounting.store(false);
    delete p;
    std::free(q);
    REQUIRE(gNews.load() - n0 == 1);
#if MW909_COUNT_MALLOC
    REQUIRE(gMallocs.load() - m0 >= 2);
#else
    (void)m0;
#endif
}
