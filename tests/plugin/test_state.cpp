// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// State save / restore (docs/design/plugin.md §3): exact round trips, stable
// bytes, hostile input, versions, and the restore-then-bounce path.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "TestSupport.h"

using namespace plugintest;

namespace {

// Put every part of the state somewhere other than its default.
void randomiseEverything(mw909Processor& p, Rng& rng) {
    randomiseParameters(p, rng);

    auto bank = std::make_unique<mw909::seq::PatternBank>();
    for (auto& pattern : bank->patterns)
        if (randInt(rng, 0, 3) != 0) pattern = randomPattern(rng);  // leave some empty
    bank->chain = randomChain(rng);
    for (auto& track : bank->tracks)
        if (randInt(rng, 0, 2) != 0) track = randomSongTrack(rng);
    p.patterns().replaceBank(*bank);
    p.patterns().selectPattern(randInt(rng, 0, mw909::seq::kNumPatterns - 1));
    p.setUnitSeed(static_cast<std::uint32_t>(rng()));
    for (int s = 0; s < mw909::kNumRomSlots; ++s) {
        if (randInt(rng, 0, 2) != 0)
            p.patterns().setUserRom(static_cast<mw909::RomSlot>(s), randomRom(rng),
                                    "rom " + juce::String(s) + " <&>\"'");
        else
            p.patterns().clearUserRom(static_cast<mw909::RomSlot>(s));
    }

    if (randInt(rng, 0, 1) == 0) p.midiLearn().clear();
    const auto& ids = mw909::plugin::allParameterIds();
    for (int i = 0; i < 12; ++i)
        p.midiLearn().assign(randInt(rng, 0, 119),
                             juce::String(ids[static_cast<std::size_t>(randInt(rng, 0, static_cast<int>(ids.size()) - 1))]));

    UiState ui;
    ui.scale = 0.6f + 1.4f * rand01(rng);
    ui.view = randInt(rng, 0, 3);
    ui.selectedTrack = randInt(rng, 0, mw909::seq::kNumTracks - 1);
    ui.stepEditMode = randInt(rng, 0, 3);
    ui.page = randInt(rng, 0, 3);
    ui.authentic = randInt(rng, 0, 1) == 1;
    // XML-hostile characters and a non-ASCII name (as UTF-8 escapes: a plain
    // char* literal with 8-bit characters trips JUCE's ASCII assertion).
    ui.presetNames = {"Kit <one> & \"two\"", juce::String(juce::CharPointer_UTF8("Bank \xc3\xa4\xc3\xb6\xc3\xbc")),
                      "Full 'three'"};
    p.setUiState(ui);
}

// Every piece of state, compared exactly.
void requireSameState(mw909Processor& a, mw909Processor& b) {
    for (const auto& id : mw909::plugin::allParameterIds()) {
        const float va = raw(a, juce::String(id));
        const float vb = raw(b, juce::String(id));
        INFO(id << ": " << va << " vs " << vb);
        REQUIRE(std::memcmp(&va, &vb, sizeof(float)) == 0);
        REQUIRE(param(a, juce::String(id)).getValue() == param(b, juce::String(id)).getValue());
    }
    REQUIRE(sameBank(a.patterns().bank(), b.patterns().bank()));  // bit for bit, chain included
    REQUIRE(a.patterns().selectedPattern() == b.patterns().selectedPattern());
    REQUIRE(a.unitSeed() == b.unitSeed());
    REQUIRE(a.midiLearn().getMap() == b.midiLearn().getMap());
    REQUIRE(a.uiState() == b.uiState());
    REQUIRE(a.getCurrentProgram() == b.getCurrentProgram());
    for (int s = 0; s < mw909::kNumRomSlots; ++s)
        REQUIRE(a.userRom(static_cast<mw909::RomSlot>(s)) == b.userRom(static_cast<mw909::RomSlot>(s)));
}

bool isDefaultState(mw909Processor& p) {
    auto fresh = makeProcessor(kRate, kBlock, false);
    return saveState(p) == saveState(*fresh);
}

}  // namespace

TEST_CASE("state: random state round-trips exactly into a fresh instance", "[plugin][state]") {
    Rng rng(1U);
    for (int round = 0; round < 8; ++round) {
        auto a = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*a, rng);
        const juce::MemoryBlock saved = saveState(*a);
        REQUIRE(saved.getSize() > 8);

        auto b = makeProcessor(kRate, kBlock, false);
        loadState(*b, saved);
        REQUIRE(b->lastStateLoadStatus() == state::ReadStatus::Ok);
        requireSameState(*a, *b);
    }
}

TEST_CASE("state: save, load, save gives identical bytes", "[plugin][state]") {
    Rng rng(2U);
    for (int round = 0; round < 6; ++round) {
        auto a = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*a, rng);
        const juce::MemoryBlock first = saveState(*a);

        // Into a fresh instance...
        auto b = makeProcessor(kRate, kBlock, false);
        loadState(*b, first);
        REQUIRE(saveState(*b) == first);

        // ...into an instance holding other state...
        auto c = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*c, rng);
        loadState(*c, first);
        REQUIRE(saveState(*c) == first);

        // ...and back into the instance it came from.
        loadState(*a, first);
        REQUIRE(saveState(*a) == first);
        REQUIRE(saveState(*a) == first);  // saving is repeatable
    }
}

TEST_CASE("state: a fresh instance saves the same bytes every time", "[plugin][state]") {
    auto a = makeProcessor(kRate, kBlock, false);
    auto b = makeProcessor(kRate, kBlock, false);
    REQUIRE(saveState(*a) == saveState(*b));
}

TEST_CASE("state: the saved tree has the documented shape", "[plugin][state]") {
    auto p = makeProcessor(kRate, kBlock, false);
    Rng rng(3U);
    p->patterns().setPattern(7, randomPattern(rng));
    p->patterns().setSongTrack(5, randomSongTrack(rng));
    p->patterns().setUserRom(mw909::RomSlot::Ride, randomRom(rng), "ride.wav");
    p->midiLearn().assign(74, "sd_tone");

    const auto xml = xmlFromState(saveState(*p));
    REQUIRE(xml != nullptr);
    CHECK(xml->hasTagName("mw909"));
    CHECK(xml->getIntAttribute("stateVersion") == 1);
    CHECK(xml->getIntAttribute("unitSeed") == 909);

    const auto* params = xml->getChildByName("PARAMETERS");
    REQUIRE(params != nullptr);
    CHECK(params->getNumChildElements() == 608);
    CHECK(params->getFirstChildElement()->getStringAttribute("id") == "bd_tune");

    const auto* sequencer = xml->getChildByName("sequencer");
    REQUIRE(sequencer != nullptr);
    CHECK(sequencer->getIntAttribute("patternFormat") == static_cast<int>(mw909::seq::kPatternFormat));
    CHECK(sequencer->getIntAttribute("patternSize") == static_cast<int>(sizeof(mw909::seq::Pattern)));
    CHECK(sequencer->getIntAttribute("songTrackSize") == static_cast<int>(sizeof(mw909::seq::SongTrack)));
    // Only non-empty patterns and tracks are stored.
    int stored = 0, tracks = 0;
    bool sawSeven = false;
    const mw909::seq::Pattern empty{};
    for (const auto* child : sequencer->getChildIterator()) {
        if (child->hasTagName("track")) {
            ++tracks;
            CHECK(child->getIntAttribute("index", -1) >= 0);
            CHECK(child->getIntAttribute("index", -1) < mw909::seq::kNumSongTracks);
            continue;
        }
        REQUIRE(child->hasTagName("pattern"));
        ++stored;
        const int index = child->getIntAttribute("index", -1);
        REQUIRE(index >= 0);
        REQUIRE(index < mw909::seq::kNumPatterns);
        CHECK_FALSE(p->patterns().pattern(index) == empty);
        sawSeven = sawSeven || index == 7;
    }
    int nonEmpty = 0;
    for (int i = 0; i < mw909::seq::kNumPatterns; ++i)
        if (!(p->patterns().pattern(i) == empty)) ++nonEmpty;
    CHECK(stored == nonEmpty);
    CHECK(sawSeven);
    int nonEmptyTracks = 0;
    for (int i = 0; i < mw909::seq::kNumSongTracks; ++i)
        if (!(p->patterns().songTrack(i) == mw909::seq::SongTrack{})) ++nonEmptyTracks;
    CHECK(tracks == nonEmptyTracks);
    CHECK(tracks >= 1);

    const auto* roms = xml->getChildByName("userRoms");
    REQUIRE(roms != nullptr);
    REQUIRE(roms->getNumChildElements() == 1);
    CHECK(roms->getFirstChildElement()->getIntAttribute("slot") == static_cast<int>(mw909::RomSlot::Ride));
    CHECK(roms->getFirstChildElement()->getStringAttribute("name") == "ride.wav");

    const auto* midi = xml->getChildByName("midiLearn");
    REQUIRE(midi != nullptr);
    CHECK(midi->getChildByAttribute("cc", "74") != nullptr);
    CHECK(midi->getFirstChildElement()->getIntAttribute("cc") == 9);  // sorted; the factory map starts at CC 9
    REQUIRE(xml->getChildByName("ui") != nullptr);
    CHECK(xml->getChildByName("ui")->hasAttribute("authentic"));
}

TEST_CASE("state: garbage never crashes and falls back to defaults", "[plugin][state]") {
    Rng rng(4U);
    auto p = makeProcessor(kRate, kBlock, true);
    Host host(*p);

    const auto expectDefaultsAfter = [&](const void* data, int size, const char* what) {
        randomiseEverything(*p, rng);
        REQUIRE_FALSE(isDefaultState(*p));
        p->setStateInformation(data, size);
        INFO(what);
        REQUIRE(isDefaultState(*p));
        REQUIRE(p->lastStateLoadStatus() != state::ReadStatus::Ok);
        host.process(256);  // and the audio side is still fine
        REQUIRE(host.allFinite(256));
    };

    SECTION("null and empty") {
        expectDefaultsAfter(nullptr, 0, "null");
        expectDefaultsAfter(nullptr, 100, "null with a size");
        const char byte = 'x';
        expectDefaultsAfter(&byte, 0, "zero size");
        expectDefaultsAfter(&byte, -5, "negative size");
    }

    SECTION("random bytes of many sizes") {
        for (int size : {1, 2, 7, 8, 9, 16, 100, 1000, 65536}) {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            for (auto& b : bytes) b = static_cast<std::uint8_t>(randInt(rng, 0, 255));
            expectDefaultsAfter(bytes.data(), size, "random bytes");
        }
    }

    SECTION("a valid header in front of garbage") {
        for (int size : {9, 64, 5000}) {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            for (auto& b : bytes) b = static_cast<std::uint8_t>(randInt(rng, 0, 255));
            const std::uint32_t magic = kXmlMagic;
            const std::uint32_t length = 0x7FFFFFF0U;  // claims far more text than there is
            std::memcpy(bytes.data(), &magic, 4);
            if (size >= 8) std::memcpy(bytes.data() + 4, &length, 4);
            expectDefaultsAfter(bytes.data(), size, "header plus garbage");
        }
    }

    SECTION("well-formed XML that is not ours") {
        juce::XmlElement other("SOMETHING_ELSE");
        other.setAttribute("stateVersion", 1);
        const auto block = stateFromXml(other);
        expectDefaultsAfter(block.getData(), static_cast<int>(block.getSize()), "foreign root");
    }

    SECTION("XML with a document type declaration is refused unparsed") {
        // The XML parser would expand entities defined in a DTD, and even read
        // the files they name. Nothing we write has one.
        auto donor = makeProcessor(kRate, kBlock, false);
        const auto xml = xmlFromState(saveState(*donor));
        const juce::String body = xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
        const juce::File secret = scratchDirectory("state-doctype").getChildFile("secret.txt");
        secret.replaceWithText("must never be read");
        for (const juce::String& doctype :
             {juce::String("<!DOCTYPE mw909 [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;\">]>"),
              juce::String("<!doctype mw909 [<!ENTITY x SYSTEM \"") + secret.getFullPathName() + "\">]>",
              juce::String("<!DOCTYPE mw909 SYSTEM \"") + secret.getFullPathName() + "\">"}) {
            const juce::String text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + doctype + body;
            juce::MemoryOutputStream out;
            out.writeInt(static_cast<int>(kXmlMagic));
            out.writeInt(static_cast<int>(text.getNumBytesAsUTF8()));
            out.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
            out.writeByte(0);
            expectDefaultsAfter(out.getData(), static_cast<int>(out.getDataSize()), "doctype");
        }
        CHECK(mw909::plugin::state::hasDoctype("<a><!DocType x></a>", 19));
        CHECK_FALSE(mw909::plugin::state::hasDoctype("<a><!-- doctype --></a>", 23));
        CHECK_FALSE(mw909::plugin::state::hasDoctype(nullptr, 0));
        CHECK_FALSE(mw909::plugin::state::hasDoctype("<!doc", 5));
    }

    SECTION("a real state truncated at every kind of place") {
        auto donor = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*donor, rng);
        const juce::MemoryBlock full = saveState(*donor);
        const int size = static_cast<int>(full.getSize());
        std::vector<int> cuts = {1, 4, 8, 9, 12, 40, 53, 97, 200, size / 4, size / 2, size - 2000};
        for (int cut = 4999; cut < size; cut += 4999) cuts.push_back(cut);  // a spread over the body
        for (int back = 2; back <= 250; ++back) cuts.push_back(size - back);  // and the whole tail
        // Just after an element closes is where the XML parser itself would accept the cut.
        const auto* bytes = static_cast<const char*>(full.getData());
        for (int i = 8; i < size - 16; ++i)
            if (std::memcmp(bytes + i, "</PARAMETERS>", 13) == 0 || std::memcmp(bytes + i, "</sequencer>", 12) == 0)
                for (int d = 10; d <= 16; ++d) cuts.push_back(i + d);
        for (int cut : cuts) {
            if (cut <= 0 || cut >= size - 1) continue;
            INFO("cut at " << cut << " of " << size);
            expectDefaultsAfter(full.getData(), cut, "truncated");
        }

        // Losing only the terminator after the text loses nothing.
        auto q = makeProcessor(kRate, kBlock, false);
        q->setStateInformation(full.getData(), size - 1);
        CHECK(q->lastStateLoadStatus() == state::ReadStatus::Ok);
        CHECK(saveState(*q) == full);
    }

    SECTION("a real state with flipped bytes") {
        auto donor = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*donor, rng);
        const juce::MemoryBlock full = saveState(*donor);
        for (int round = 0; round < 60; ++round) {
            juce::MemoryBlock damaged = full;
            for (int flips = 0; flips < 1 + round; ++flips) {
                const auto at = static_cast<std::size_t>(randInt(rng, 0, static_cast<int>(damaged.getSize()) - 1));
                static_cast<std::uint8_t*>(damaged.getData())[at] = static_cast<std::uint8_t>(randInt(rng, 0, 255));
            }
            // Whatever this parses to, it must load without crashing and
            // leave a state that saves and processes.
            loadState(*p, damaged);
            REQUIRE(saveState(*p).getSize() > 8);
            host.process(128);
            REQUIRE(host.allFinite(128));
        }
    }
}

TEST_CASE("state: a newer stateVersion is not interpreted", "[plugin][state]") {
    Rng rng(5U);
    auto donor = makeProcessor(kRate, kBlock, false);
    randomiseEverything(*donor, rng);
    auto xml = xmlFromState(saveState(*donor));
    REQUIRE(xml != nullptr);
    xml->setAttribute("stateVersion", 2);

    auto p = makeProcessor(kRate, kBlock, false);
    randomiseEverything(*p, rng);
    loadState(*p, stateFromXml(*xml));
    REQUIRE(p->lastStateLoadStatus() == state::ReadStatus::FutureVersion);
    REQUIRE(isDefaultState(*p));

    xml->setAttribute("stateVersion", 999999);
    randomiseEverything(*p, rng);
    loadState(*p, stateFromXml(*xml));
    REQUIRE(p->lastStateLoadStatus() == state::ReadStatus::FutureVersion);
    REQUIRE(isDefaultState(*p));
}

TEST_CASE("state: an older state without a version stamp is migrated", "[plugin][state]") {
    Rng rng(6U);
    auto donor = makeProcessor(kRate, kBlock, false);
    randomiseEverything(*donor, rng);
    auto xml = xmlFromState(saveState(*donor));
    REQUIRE(xml != nullptr);
    xml->removeAttribute("stateVersion");  // the v0 layout

    auto p = makeProcessor(kRate, kBlock, false);
    loadState(*p, stateFromXml(*xml));
    REQUIRE(p->lastStateLoadStatus() == state::ReadStatus::Ok);
    requireSameState(*donor, *p);

    // What it saves next is the current version.
    const auto resaved = xmlFromState(saveState(*p));
    REQUIRE(resaved != nullptr);
    REQUIRE(resaved->getIntAttribute("stateVersion") == state::kStateVersion);

    // The migration function itself.
    juce::ValueTree v0("mw909");
    const juce::ValueTree v1 = state::migrate(v0, 0);
    REQUIRE(static_cast<int>(v1.getProperty("stateVersion")) == 1);
}

TEST_CASE("state: damaged parts fall back one by one", "[plugin][state]") {
    Rng rng(7U);
    auto donor = makeProcessor(kRate, kBlock, false);
    randomiseEverything(*donor, rng);
    // Make sure patterns 3 and 9 hold something.
    donor->patterns().setPattern(3, randomPattern(rng));
    donor->patterns().setPattern(9, randomPattern(rng));
    const juce::MemoryBlock good = saveState(*donor);

    const auto patternElement = [](juce::XmlElement& xml, int index) -> juce::XmlElement* {
        for (auto* child : xml.getChildByName("sequencer")->getChildIterator())
            if (child->getIntAttribute("index", -1) == index) return child;
        return nullptr;
    };

    SECTION("one unreadable pattern blob leaves that pattern empty") {
        auto xml = xmlFromState(good);
        REQUIRE(patternElement(*xml, 3) != nullptr);
        patternElement(*xml, 3)->setAttribute("data", "!!!! not base64 !!!!");
        auto p = makeProcessor(kRate, kBlock, false);
        loadState(*p, stateFromXml(*xml));
        CHECK(p->patterns().pattern(3) == mw909::seq::Pattern{});
        CHECK(p->patterns().pattern(9) == donor->patterns().pattern(9));
        CHECK(raw(*p, "bd_level") == raw(*donor, "bd_level"));
    }

    SECTION("a blob of the wrong size is refused") {
        auto xml = xmlFromState(good);
        const juce::String data = patternElement(*xml, 3)->getStringAttribute("data");
        patternElement(*xml, 3)->setAttribute("data", data.substring(0, data.length() / 2 / 4 * 4));
        auto p = makeProcessor(kRate, kBlock, false);
        loadState(*p, stateFromXml(*xml));
        CHECK(p->patterns().pattern(3) == mw909::seq::Pattern{});
        CHECK(p->patterns().pattern(9) == donor->patterns().pattern(9));
    }

    SECTION("a different pattern size or format keeps the default pattern memory") {
        for (const char* attribute : {"patternSize", "patternFormat"}) {
            auto xml = xmlFromState(good);
            xml->getChildByName("sequencer")->setAttribute(attribute, 12345);
            auto p = makeProcessor(kRate, kBlock, false);
            auto fresh = makeProcessor(kRate, kBlock, false);
            loadState(*p, stateFromXml(*xml));
            INFO(attribute);
            CHECK(sameBank(p->patterns().bank(), fresh->patterns().bank()));
            CHECK(raw(*p, "bd_level") == raw(*donor, "bd_level"));  // parameters still load
        }
    }

    SECTION("out-of-range fields inside a blob are forced legal") {
        auto xml = xmlFromState(good);
        mw909::seq::Pattern bad = donor->patterns().pattern(9);
        bad.length = 0;
        bad.scale = static_cast<mw909::seq::Scale>(200);
        bad.shuffle = 255;
        bad.trackLength[2] = 200;
        patternElement(*xml, 9)->setAttribute("data", juce::Base64::toBase64(&bad, sizeof(bad)));
        auto p = makeProcessor(kRate, kBlock, true);
        loadState(*p, stateFromXml(*xml));
        const auto& loaded = p->patterns().pattern(9);
        CHECK(loaded.length >= 1);
        CHECK(loaded.length <= mw909::seq::kMaxSteps);
        CHECK(static_cast<int>(loaded.scale) < static_cast<int>(mw909::seq::Scale::kNumScales));
        CHECK(loaded.shuffle <= 100);
        CHECK(loaded.trackLength[2] <= mw909::seq::kMaxSteps);
        // And playing it is safe.
        p->patterns().selectPattern(9, true);
        Host host(*p);
        host.run(48000, 512);
        CHECK(host.allFinite(512));
    }

    SECTION("bad parameter entries fall back or clamp") {
        auto xml = xmlFromState(good);
        auto* params = xml->getChildByName("PARAMETERS");
        for (auto* child : params->getChildIterator()) {
            const juce::String id = child->getStringAttribute("id");
            if (id == "bd_level") child->setAttribute("value", "nan");
            if (id == "sd_level") child->setAttribute("value", "banana");
            if (id == "tempo") child->setAttribute("value", "1e9");
            if (id == "master_volume") child->setAttribute("value", "-inf");
            if (id == "bd_out") child->setAttribute("value", "57");
            if (id == "cr_tune") child->setAttribute("value", "");
        }
        auto* stray = params->createNewChildElement("PARAM");
        stray->setAttribute("id", "no_such_parameter");
        stray->setAttribute("value", "0.5");
        params->removeChildElement(params->getChildByAttribute("id", "total_accent"), true);

        auto p = makeProcessor(kRate, kBlock, false);
        auto fresh = makeProcessor(kRate, kBlock, false);
        setParam(*p, "total_accent", 0.123f);
        loadState(*p, stateFromXml(*xml));
        CHECK(raw(*p, "bd_level") == raw(*fresh, "bd_level"));
        CHECK(raw(*p, "sd_level") == raw(*fresh, "sd_level"));
        CHECK(raw(*p, "tempo") == 400.0f);
        CHECK(raw(*p, "master_volume") == raw(*fresh, "master_volume"));
        CHECK(raw(*p, "bd_out") == 15.0f);
        CHECK(raw(*p, "cr_tune") == raw(*fresh, "cr_tune"));
        CHECK(raw(*p, "total_accent") == raw(*fresh, "total_accent"));  // missing: default, not left over
        CHECK(raw(*p, "ch_level") == raw(*donor, "ch_level"));
    }

    SECTION("bad MIDI-learn, UI and selection entries are dropped or clamped") {
        auto xml = xmlFromState(good);
        auto* midi = xml->getChildByName("midiLearn");
        midi->deleteAllChildElements();
        auto add = [midi](const char* cc, const char* id) {
            auto* m = midi->createNewChildElement("map");
            m->setAttribute("cc", cc);
            m->setAttribute("param", id);
        };
        add("20", "bd_level");
        add("500", "sd_level");
        add("-1", "sd_level");
        add("21", "no_such_parameter");
        add("abc", "sd_level");
        add("125", "sd_level");  // channel-mode controllers cannot be bound
        auto* ui = xml->getChildByName("ui");
        ui->setAttribute("scale", "17");
        ui->setAttribute("view", "9");
        ui->setAttribute("track", "-4");
        ui->setAttribute("page", "x");
        xml->getChildByName("sequencer")->setAttribute("selected", 4000);
        xml->setAttribute("unitSeed", "-12");
        xml->setAttribute("program", "100000000");

        auto p = makeProcessor(kRate, kBlock, false);
        loadState(*p, stateFromXml(*xml));
        const auto map = p->midiLearn().getMap();
        REQUIRE(map.size() == 1U);
        CHECK(map[0].first == 20);
        CHECK(map[0].second == "bd_level");
        const UiState loaded = p->uiState();
        CHECK(loaded.scale == mw909::plugin::kMaxUiScale);
        CHECK(loaded.view >= 0);
        CHECK(loaded.view <= 3);
        CHECK(loaded.selectedTrack >= 0);
        CHECK(loaded.page >= 0);
        CHECK(loaded.page <= 3);
        CHECK(p->patterns().selectedPattern() >= 0);
        CHECK(p->patterns().selectedPattern() < mw909::seq::kNumPatterns);
        CHECK(p->unitSeed() == mw909::plugin::kDefaultUnitSeed);
        CHECK(p->getCurrentProgram() >= 0);
        CHECK(p->getCurrentProgram() < p->getNumPrograms());
    }
}

namespace {

// A bank whose pattern 5 plays the rim shot on every step and nothing else,
// and whose other patterns are empty.
std::unique_ptr<mw909::seq::PatternBank> rimShotBank() {
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    for (int s = 0; s < 16; ++s) bank->patterns[5].at(0, mw909::RS, s).setLevel(mw909::seq::StepLevel::Normal);
    return bank;
}

}  // namespace

TEST_CASE("state: an offline bounce right after a load plays the restored patterns", "[plugin][state]") {
    auto donor = makeProcessor(kRate, kBlock, false);
    donor->patterns().replaceBank(*rimShotBank());
    donor->patterns().selectPattern(5);
    const juce::MemoryBlock saved = saveState(*donor);

    SECTION("loaded on the message thread, no timer, no service call") {
        auto p = makeProcessor(kRate, kBlock, true);
        Host host(*p);
        // Play the default memory first so the audio side is in full swing.
        host.run(24000, 512);
        REQUIRE(hits(*p, mw909::BD) > 0U);
        const auto bdBefore = hits(*p, mw909::BD);
        const auto clBefore = hits(*p, mw909::RS);

        loadState(*p, saved);
        host.run(48000, 512);  // straight into the next blocks
        CHECK(hits(*p, mw909::RS) - clBefore >= 7U);   // one bar of sixteenths at 120 BPM = 2 s
        CHECK(hits(*p, mw909::BD) == bdBefore);        // nothing of the old memory plays
        CHECK(p->playState().selectedPattern.load() == 5);
        CHECK(sameBank(p->machineForTests().sequencer().bank(), *rimShotBank()));
    }

    SECTION("loaded on a background thread while nothing pumps messages") {
        auto p = makeProcessor(kRate, kBlock, true);
        Host host(*p);
        host.run(4800, 512);
        const auto bdBefore = hits(*p, mw909::BD);
        const auto clBefore = hits(*p, mw909::RS);

        int bankCallbacks = 0, selectionCallbacks = 0;
        struct Counter final : mw909::plugin::PatternModel::Listener {
            int& bank;
            int& selection;
            Counter(int& b, int& s) : bank(b), selection(s) {}
            void bankChanged() override { ++bank; }
            void selectionChanged() override { ++selection; }
        } counter(bankCallbacks, selectionCallbacks);
        p->patterns().addListener(&counter);

        juce::MemoryBlock resaved;
        std::thread loader([&] {
            loadState(*p, saved);
            resaved = saveState(*p);  // a save on that thread already sees the new state
        });
        loader.join();

        CHECK(resaved == saved);
        CHECK(bankCallbacks == 0);  // listeners are message-thread only
        CHECK(p->patterns().hasStagedRestore());

        host.run(48000, 512);
        CHECK(hits(*p, mw909::RS) - clBefore >= 7U);
        CHECK(hits(*p, mw909::BD) == bdBefore);

        // The message thread catches up on its next service pass.
        p->serviceMessageThread();
        CHECK_FALSE(p->patterns().hasStagedRestore());
        CHECK(bankCallbacks == 1);
        CHECK(selectionCallbacks == 1);
        CHECK(sameBank(p->patterns().bank(), *rimShotBank()));
        CHECK(p->patterns().selectedPattern() == 5);
        CHECK(saveState(*p) == saved);
        p->patterns().removeListener(&counter);
    }

    SECTION("loaded before the first prepareToPlay") {
        auto p = makeProcessor(kRate, kBlock, false);
        loadState(*p, saved);
        p->prepareToPlay(44100.0, 256);
        Host host(*p);
        host.sampleRate = 44100.0;
        host.run(44100, 256);
        CHECK(hits(*p, mw909::RS) >= 7U);
        CHECK(hits(*p, mw909::BD) == 0U);
    }
}

TEST_CASE("state: an edit made before a background load does not leak into it", "[plugin][state]") {
    auto donor = makeProcessor(kRate, kBlock, false);
    donor->patterns().replaceBank(*rimShotBank());
    const juce::MemoryBlock saved = saveState(*donor);

    auto p = makeProcessor(kRate, kBlock, true);
    Host host(*p);
    Rng rng(8U);
    // Queue single edits (older generation), then load on another thread.
    for (int i = 0; i < 20; ++i) p->patterns().setPattern(i, randomPattern(rng));
    std::thread loader([&] { loadState(*p, saved); });
    loader.join();

    host.process(256);
    CHECK(sameBank(p->machineForTests().sequencer().bank(), *rimShotBank()));
    p->serviceMessageThread();
    host.process(256);
    p->serviceMessageThread();
    CHECK(sameBank(p->patterns().bank(), *rimShotBank()));
    CHECK(sameBank(p->machineForTests().sequencer().bank(), *rimShotBank()));

    // An edit made after the load, before any service call, lands on the new bank.
    std::thread again([&] { loadState(*p, saved); });
    again.join();
    mw909::seq::Pattern edited = rimShotBank()->patterns[5];
    edited.at(0, mw909::BD, 0).setLevel(mw909::seq::StepLevel::Accent);
    p->patterns().setPattern(5, edited);
    host.process(256);
    CHECK(p->machineForTests().sequencer().bank().patterns[5] == edited);
    CHECK(p->patterns().pattern(5) == edited);
    CHECK(p->patterns().pattern(6) == mw909::seq::Pattern{});
}

namespace {

void toggleFirstStep(mw909::seq::Pattern& x) {
    auto& step = x.at(0, mw909::BD, 0);
    step.setLevel(step.on() ? mw909::seq::StepLevel::Off : mw909::seq::StepLevel::Accent);
}

}  // namespace

TEST_CASE("state: edits right after a background load apply to the loaded data", "[plugin][state]") {
    auto p = makeProcessor(kRate, kBlock, true);
    Host host(*p);
    p->setMidiExportDirectory(scratchDirectory("edit-after-load"));
    Rng rng(1234U);
    // The session that will be restored (B), then different model data (A).
    const auto patB = randomPattern(rng);
    const auto chainB = randomChain(rng);
    const auto trackB = randomSongTrack(rng);
    p->patterns().setPattern(5, patB);
    p->patterns().setPattern(7, patB);
    p->patterns().setChain(chainB);
    p->patterns().setSongTrack(2, trackB);
    p->serviceMessageThread();
    const juce::MemoryBlock stateB = saveState(*p);
    const juce::File midiB = p->exportPatternToMidiFile(7, 0);
    REQUIRE(midiB.existsAsFile());
    const auto patA = randomPattern(rng);
    p->patterns().setPattern(5, patA);
    p->patterns().setPattern(7, patA);
    p->patterns().setChain(randomChain(rng));
    p->patterns().setSongTrack(2, randomSongTrack(rng));
    p->serviceMessageThread();
    REQUIRE(!(p->patterns().pattern(5) == patB));

    // The host restores B on a worker thread; the editor acts before the timer runs.
    std::thread loader([&] { loadState(*p, stateB); });
    loader.join();
    REQUIRE(p->patterns().hasStagedRestore());
    // A read of the session's data already sees B.
    CHECK(p->patterns().patternCopy(5) == patB);

    SECTION("pattern edit") {
        CHECK(p->patterns().edit(5, toggleFirstStep));
        auto expected = patB;
        toggleFirstStep(expected);
        CHECK(p->patterns().pattern(5) == expected);
        host.process(256);
        p->serviceMessageThread();
        host.process(256);
        CHECK(p->patterns().pattern(5) == expected);
        CHECK(p->machineForTests().sequencer().bank().patterns[5] == expected);
        // Everything else is B's.
        CHECK(p->patterns().pattern(7) == patB);
        CHECK(p->patterns().chain() == chainB);
    }
    SECTION("chain and song track edits") {
        CHECK(p->patterns().editChain([](mw909::seq::Chain& c) { c.entries[0] = 42; }));
        CHECK(p->patterns().editSongTrack(2, [](mw909::seq::SongTrack& t) { t.entries[0] = 17; }));
        auto chain = chainB;
        chain.entries[0] = 42;
        auto track = trackB;
        track.entries[0] = 17;
        CHECK(p->patterns().chain() == chain);
        CHECK(p->patterns().songTrack(2) == track);
        CHECK(p->patterns().pattern(5) == patB);
    }
    SECTION("an edit that declines changes nothing but still adopts the load") {
        CHECK_FALSE(p->patterns().edit(5, [](mw909::seq::Pattern& x) {
            toggleFirstStep(x);
            return false;
        }));
        CHECK_FALSE(p->patterns().hasStagedRestore());
        CHECK(p->patterns().pattern(5) == patB);
    }
    SECTION("MIDI file import keeps the loaded pattern's other variation") {
        REQUIRE(p->importMidiFile(midiB, 7, 0));
        CHECK(p->patterns().pattern(7).steps[1] == patB.steps[1]);
    }
    SECTION("MIDI file export writes the loaded pattern") {
        const juce::File out = p->exportPatternToMidiFile(7, 0);
        REQUIRE(out.existsAsFile());
        juce::MemoryBlock a, b;
        REQUIRE(out.loadFileAsData(a));
        REQUIRE(midiB.loadFileAsData(b));
        CHECK(a == b);
    }
}

TEST_CASE("state: a load that lands while an edit runs redoes the edit on the loaded data", "[plugin][state]") {
    auto p = makeProcessor(kRate, kBlock, false);
    Rng rng(77U);
    const auto patB = randomPattern(rng);
    p->patterns().setPattern(3, patB);
    p->serviceMessageThread();
    const juce::MemoryBlock stateB = saveState(*p);
    p->patterns().setPattern(3, randomPattern(rng));
    p->serviceMessageThread();

    int runs = 0;
    CHECK(p->patterns().edit(3, [&](mw909::seq::Pattern& x) {
        if (runs++ == 0) {
            // The host loads B on another thread while the edit is in progress.
            std::thread loader([&] { loadState(*p, stateB); });
            loader.join();
        }
        toggleFirstStep(x);
    }));
    CHECK(runs == 2);
    auto expected = patB;
    toggleFirstStep(expected);
    CHECK(p->patterns().pattern(3) == expected);
    CHECK_FALSE(p->patterns().hasStagedRestore());
}

TEST_CASE("state: saving and loading from several threads while audio runs", "[plugin][state][threads]") {
    auto p = makeProcessor(kRate, kBlock, true);
    Rng rng(9U);
    std::vector<juce::MemoryBlock> states;
    for (int i = 0; i < 4; ++i) {
        auto donor = makeProcessor(kRate, kBlock, false);
        randomiseEverything(*donor, rng);
        states.push_back(saveState(*donor));
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> finite{true};

    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        midi.ensureSize(2048);
        int n = 0;
        while (!stop.load()) {
            midi.clear();
            if ((++n & 7) == 0) midi.addEvent(juce::MidiMessage::noteOn(10, 36 + (n % 20), juce::uint8{100}), 0);
            p->processBlock(buffer, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    if (!std::isfinite(buffer.getSample(ch, i))) finite.store(false);
        }
    });
    std::thread hostThread([&] {
        int n = 0;
        while (!stop.load()) {
            loadState(*p, states[static_cast<std::size_t>(n++ % 4)]);
            juce::MemoryBlock out = saveState(*p);
            if (out.getSize() <= 8) finite.store(false);
            if ((n % 5) == 0) p->setCurrentProgram(n % std::max(1, p->getNumPrograms()));
        }
    });

    // The message thread: edits, service passes, its own saves and loads. It must
    // keep getting through while the other two threads hammer the same state:
    // more than kMinRounds rounds in 1.5 s. Under ASan / TSan every round (up to
    // three state round trips through the codec, contending with the host
    // thread's) runs some 35 to 55 times slower, so those builds keep going past
    // the 1.5 s until the minimum is met, within kRoundBudgetMs: still a hard
    // bound on progress, just not one set by the instrumentation's overhead.
    constexpr int kMinRounds = 10;
    constexpr juce::uint32 kRunMs = 1500U;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    constexpr juce::uint32 kRoundBudgetMs = 30000U;
#else
    constexpr juce::uint32 kRoundBudgetMs = kRunMs;
#endif
    Rng edits(10U);
    const auto start = juce::Time::getMillisecondCounter();
    int rounds = 0;
    for (;;) {
        const auto elapsed = juce::Time::getMillisecondCounter() - start;
        if (elapsed >= kRunMs && (rounds > kMinRounds || elapsed >= kRoundBudgetMs)) break;
        p->patterns().setPattern(randInt(edits, 0, 95), randomPattern(edits));
        p->patterns().selectPattern(randInt(edits, 0, 95), (rounds & 1) != 0);
        if ((rounds % 5) == 0) p->patterns().setSongTrack(randInt(edits, 0, 7), randomSongTrack(edits));
        if ((rounds % 13) == 0) p->patterns().setUserRom(mw909::RomSlot::Crash, randomRom(edits), "crash");
        p->serviceMessageThread();
        if ((rounds % 7) == 0) loadState(*p, states[static_cast<std::size_t>(rounds % 4)]);
        if ((rounds % 3) == 0) (void) saveState(*p);
        if ((rounds % 11) == 0) p->audition(mw909::CP, true);
        ++rounds;
    }
    stop.store(true);
    audio.join();
    hostThread.join();

    CHECK(finite.load());
    INFO("message-thread rounds: " << rounds);
    CHECK(rounds > kMinRounds);

    // Once things settle, the model and the audio side agree again.
    Host host(*p);
    for (int i = 0; i < 20; ++i) {
        host.process(256);
        p->serviceMessageThread();
    }
    CHECK(p->patterns().numDirty() == 0);
    CHECK(sameBank(p->patterns().bank(), p->machineForTests().sequencer().bank()));
}

namespace {

juce::MemoryBlock wrapXmlText(const juce::String& text) {
    juce::MemoryOutputStream out;
    out.writeInt(static_cast<int>(kXmlMagic));
    out.writeInt(static_cast<int>(text.getNumBytesAsUTF8()));
    out.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
    out.writeByte(0);
    return out.getMemoryBlock();
}

}  // namespace

TEST_CASE("state: deeply nested or oversized XML is refused without being parsed", "[plugin][state]") {
    auto p = makeProcessor(kRate, kBlock, true);
    Host host(*p);
    Rng rng(41U);
    const auto fresh = saveState(*makeProcessor(kRate, kBlock, false));

    SECTION("nesting deep enough to exhaust the stack of a recursive parser") {
        // The XML reader recurses once per level; a crafted session or a
        // preset file shared online must not be able to take the host down.
        juce::MemoryOutputStream text;
        text << "<mw909 stateVersion=\"1\">";
        for (int i = 0; i < 400000; ++i) text << "<a>";
        for (int i = 0; i < 400000; ++i) text << "</a>";
        text << "</mw909>";
        const auto block = wrapXmlText(text.toString());
        p->patterns().setPattern(3, randomPattern(rng));
        loadState(*p, block);
        CHECK(p->lastStateLoadStatus() == state::ReadStatus::Corrupt);
        CHECK(saveState(*p) == fresh);
        host.process(256);
        CHECK(host.allFinite(256));
    }

    SECTION("nesting is measured, not guessed: our own deepest level still loads") {
        auto donor = makeProcessor(kRate, kBlock, false);
        donor->midiLearn().assign(3, "sd_tone");
        const auto saved = saveState(*donor);
        loadState(*p, saved);
        CHECK(p->lastStateLoadStatus() == state::ReadStatus::Ok);
        CHECK(saveState(*p) == saved);
        // Quotes, comments and CDATA do not fool the depth count.
        const auto exceeds = [](const char* text, int depth) {
            return state::xmlNestingExceeds(text, std::strlen(text), depth);
        };
        CHECK_FALSE(exceeds("<a x=\"<b><c>\" y='<d>'><!-- <d><e> --><![CDATA[<f><g>]]><?pi <h>?></a>", 1));
        CHECK(exceeds("<a><b><c/></b></a>", 1));
        CHECK_FALSE(exceeds("<a><b><c/></b></a>", 2));
        CHECK(exceeds("<a><b><c></c></b></a>", 2));
        CHECK(exceeds("<a><b><c x=\"/\">", 2));  // unclosed counts too
    }

    SECTION("a state far larger than anything this plugin writes") {
        for (const std::size_t size : {std::size_t{2u * 1024u * 1024u + 64u}, std::size_t{24u * 1024u * 1024u}}) {
            juce::MemoryOutputStream text;
            text << "<mw909 stateVersion=\"1\"><PARAMETERS>";
            while (text.getDataSize() < size) text << "<PARAM id=\"bd_level\" value=\"0.5\"/>";
            text << "</PARAMETERS></mw909>";
            const auto block = wrapXmlText(text.toString());
            const double start = juce::Time::getMillisecondCounterHiRes();
            p->patterns().setPattern(3, randomPattern(rng));
            loadState(*p, block);
            INFO("size " << size);
            CHECK(juce::Time::getMillisecondCounterHiRes() - start < 2000.0);
            CHECK(p->lastStateLoadStatus() == state::ReadStatus::Corrupt);
            CHECK(saveState(*p) == fresh);
        }
        // The largest state this plugin can write (every pattern, track and ROM
        // in use) is well inside the limit.
        auto big = makeProcessor(kRate, kBlock, false);
        auto bank = std::make_unique<mw909::seq::PatternBank>();
        for (auto& pattern : bank->patterns) {
            pattern = randomPattern(rng);
            for (auto& variation : pattern.steps)
                for (auto& track : variation)
                    for (auto& step : track) step.bits = static_cast<std::uint8_t>(rng() | 1U);
        }
        for (auto& track : bank->tracks) track = randomSongTrack(rng);
        bank->chain = randomChain(rng);
        big->patterns().replaceBank(*bank);
        for (int s = 0; s < mw909::kNumRomSlots; ++s)
            big->patterns().setUserRom(static_cast<mw909::RomSlot>(s), randomRom(rng), juce::String::repeatedString("n", 128));
        const auto saved = saveState(*big);
        INFO("largest state " << saved.getSize() << " bytes");
        CHECK(saved.getSize() < state::kMaxStateBytes / 2);
        loadState(*p, saved);
        CHECK(p->lastStateLoadStatus() == state::ReadStatus::Ok);
        CHECK(saveState(*p) == saved);
    }

    SECTION("text that is not UTF-8 is refused before the XML reader sees it") {
        auto donor = makeProcessor(kRate, kBlock, false);
        const juce::MemoryBlock good = saveState(*donor);
        for (const char* bad : {"\xC0\xAF", "\xFF\xFE", "\xED\xA0\x80", "\xF8\x88\x80\x80\x80", "\x80"}) {
            juce::MemoryBlock damaged = good;
            // Into a preset name, where text is free-form.
            auto* bytes = static_cast<char*>(damaged.getData());
            const std::string text(bytes + 8, damaged.getSize() - 9);
            const auto at = text.find("presetName=\"");
            REQUIRE(at != std::string::npos);
            const std::size_t where = 8 + at + 12;
            std::memcpy(bytes + where, bad, std::strlen(bad));
            p->patterns().setPattern(3, randomPattern(rng));
            loadState(*p, damaged);
            INFO("bytes " << juce::String::toHexString(bad, static_cast<int>(std::strlen(bad))));
            CHECK(p->lastStateLoadStatus() == state::ReadStatus::Corrupt);
            CHECK(saveState(*p) == fresh);
        }
    }
}

TEST_CASE("state: extreme content round-trips bit for bit", "[plugin][state]") {
    // Every pattern full of arbitrary step bytes (all flag combinations), the
    // longest chain, parameters at both ends of their ranges, every controller
    // learned, UI state at its limits and preset names at their length limit.
    Rng rng(4711U);
    for (int round = 0; round < 4; ++round) {
        auto a = makeProcessor(kRate, kBlock, false);
        int i = 0;
        for (auto* parameter : a->getParameters()) parameter->setValueNotifyingHost(((i++ + round) & 1) != 0 ? 1.0f : 0.0f);

        auto bank = std::make_unique<mw909::seq::PatternBank>();
        for (auto& pattern : bank->patterns) {
            pattern = randomPattern(rng);
            for (auto& variation : pattern.steps)
                for (auto& track : variation)
                    for (auto& step : track) step.bits = static_cast<std::uint8_t>(rng());
            pattern.length = static_cast<std::uint8_t>(round % 2 == 0 ? mw909::seq::kMaxSteps : 1);
        }
        bank->chain.length = static_cast<std::uint8_t>(mw909::seq::kMaxChainLength);
        for (auto& e : bank->chain.entries) e = static_cast<std::uint8_t>(randInt(rng, 0, mw909::seq::kNumPatterns - 1));
        for (auto& track : bank->tracks) {
            track = randomSongTrack(rng);
            track.length = static_cast<std::uint16_t>(round % 2 == 0 ? mw909::seq::kMaxSongLength : 1);
        }
        a->patterns().replaceBank(*bank);
        a->patterns().selectPattern(round % 2 == 0 ? 0 : mw909::seq::kNumPatterns - 1);
        a->setUnitSeed(round % 2 == 0 ? 0xFFFFFFFFU : 0U);
        for (int s = 0; s < mw909::kNumRomSlots; ++s) {
            mw909::RomImage image;
            for (std::size_t w = 0; w < image.data.size(); ++w)
                image.data[w] = static_cast<std::uint8_t>(round % 2 == 0 ? (w * 7 + static_cast<std::size_t>(s)) % 64 : 63 * (w & 1));
            a->patterns().setUserRom(static_cast<mw909::RomSlot>(s), image,
                                     juce::String(juce::CharPointer_UTF8("\xe2\x80\x94 h\xc3\xa9 <&>.wav")));
        }

        const auto& ids = mw909::plugin::allParameterIds();
        for (int cc = 0; cc < mw909::plugin::MidiLearn::kNumControllers; ++cc)
            a->midiLearn().assign(cc, juce::String(ids[static_cast<std::size_t>(cc)]));

        UiState ui;
        ui.scale = round % 2 == 0 ? mw909::plugin::kMinUiScale : mw909::plugin::kMaxUiScale;
        ui.view = 3;
        ui.authentic = round % 2 == 0;
        ui.selectedTrack = mw909::seq::kNumTracks - 1;
        ui.stepEditMode = 3;
        ui.page = 3;
        ui.presetNames = {juce::String::repeatedString("<&>\"'", 25).substring(0, 128), juce::String(),
                          juce::String(juce::CharPointer_UTF8("\xe2\x80\x94 \xc3\xa9 \xf0\x9f\x8e\xb5"))};
        a->setUiState(ui);

        const juce::MemoryBlock saved = saveState(*a);
        auto b = makeProcessor(kRate, kBlock, false);
        loadState(*b, saved);
        REQUIRE(b->lastStateLoadStatus() == state::ReadStatus::Ok);
        requireSameState(*a, *b);
        CHECK(sameBank(b->patterns().bank(), *bank));
        CHECK(b->midiLearn().getMap().size() == static_cast<std::size_t>(mw909::plugin::MidiLearn::kNumControllers));
        CHECK(saveState(*b) == saved);
        CHECK(saveState(*b) == saved);
    }
}

TEST_CASE("state: a fresh instance is exactly factory preset 0", "[plugin][state][programs]") {
    // Load another factory preset, then preset 0 again: the session must be
    // the one a brand-new instance saves (its program, names and memory).
    const juce::MemoryBlock fresh = saveState(*makeProcessor(kRate, kBlock, false));
    auto p = makeProcessor(kRate, kBlock, false);
    REQUIRE(p->getNumPrograms() > 1);
    p->setCurrentProgram(1);
    REQUIRE(saveState(*p) != fresh);
    p->setCurrentProgram(0);
    p->serviceMessageThread();
    CHECK(saveState(*p) == fresh);
}

TEST_CASE("state: restoring a value the APVTS would round away never jumps across the range", "[plugin][state]") {
    // When a parameter holds a value within a float epsilon of the one being
    // restored, the APVTS ignores the restore and the shell nudges the
    // parameter away and back. The audio thread can run a block in between,
    // so "away" must be next door, not the other end of the range.
    auto p = makeProcessor(kRate, kBlock, false);
    setParam(*p, "master_drive", 0.5f);
    setParam(*p, "total_accent", 0.25f);
    const juce::MemoryBlock saved = saveState(*p);

    struct Recorder final : juce::AudioProcessorParameter::Listener {
        std::vector<float> seen;
        void parameterValueChanged(int, float v) override { seen.push_back(v); }
        void parameterGestureChanged(int, bool) override {}
    };
    for (const char* id : {"master_drive", "total_accent"}) {
        auto& parameter = param(*p, id);
        const float target = parameter.getValue();
        for (const bool viaFarValue : {true, false}) {
            // One float step off the saved value: "approximately equal" to the
            // APVTS. Coming from far away the APVTS copy is off too; coming
            // from the target itself only the parameter's own value is.
            if (viaFarValue) parameter.setValueNotifyingHost(0.9f);
            parameter.setValueNotifyingHost(std::nextafter(target, 2.0f));
            if (viaFarValue) REQUIRE(raw(*p, id) != parameter.convertFrom0to1(target));
            REQUIRE(parameter.getValue() != target);

            Recorder recorder;
            parameter.addListener(&recorder);
            loadState(*p, saved);
            parameter.removeListener(&recorder);

            INFO(id << (viaFarValue ? " (both off)" : " (parameter off)"));
            CHECK(parameter.getValue() == target);
            CHECK(raw(*p, id) == parameter.convertFrom0to1(target));
            for (float v : recorder.seen) CHECK(std::abs(v - target) < 0.001f);
            CHECK(saveState(*p) == saved);
        }
    }
}

TEST_CASE("state: saved at one rate and layout, loaded at another while playing", "[plugin][state]") {
    Rng rng(99U);
    auto donor = makeProcessor(44100.0, 64, true);
    REQUIRE(setLayoutAll(*donor));
    donor->prepareToPlay(44100.0, 64);
    randomiseEverything(*donor, rng);
    setParam(*donor, "seq_mode", 2.0f);
    setParam(*donor, "seq_run", 1.0f);
    const juce::MemoryBlock saved = saveState(*donor);

    for (const double rate : {48000.0, 96000.0, 192000.0}) {
        auto p = makeProcessor(rate, 1024, true);
        REQUIRE(setLayout(*p, {}));
        p->prepareToPlay(rate, 1024);
        Host host(*p);
        host.sampleRate = rate;
        setParam(*p, "seq_mode", 2.0f);
        setParam(*p, "seq_run", 1.0f);
        host.run(static_cast<int>(rate / 2), 1024);  // playing
        loadState(*p, saved);
        INFO("rate " << rate);
        CHECK(p->lastStateLoadStatus() == state::ReadStatus::Ok);
        host.run(static_cast<int>(rate), 777);  // straight on, odd block size
        CHECK(host.allFinite(777));
        CHECK(p->playState().playing.load());
        CHECK(saveState(*p) == saved);
    }
}
