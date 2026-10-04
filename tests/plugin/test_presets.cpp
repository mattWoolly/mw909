// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// PresetManager and host programs (docs/design/plugin.md §4), MIDI file
// export / import and the placeholder editor. Every file these tests write
// goes under the build tree: the user's real preset directory is never touched.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <thread>

#include "TestSupport.h"

using namespace plugintest;
using Catch::Approx;
using mw909::plugin::PresetEntry;

namespace {

struct PresetListener final : mw909::plugin::PresetManager::Listener {
    int listChanges = 0, loads = 0;
    void presetListChanged(PresetKind) override { ++listChanges; }
    void presetLoaded(PresetKind) override { ++loads; }
};

// A processor whose preset manager points at an empty scratch directory.
std::unique_ptr<mw909Processor> makeWithRoot(const juce::String& name) {
    auto p = makeProcessor();
    p->presets().setUserRoot(scratchDirectory(name));
    return p;
}

}  // namespace

TEST_CASE("presets: the default location follows the house convention", "[plugin][presets]") {
    // Looked at, not touched: nothing here creates or scans the directory.
    const juce::File root = mw909::plugin::PresetManager::defaultUserRoot();
    if (juce::SystemStats::getEnvironmentVariable("MW909_PRESET_DIR", {}).isEmpty()) {
        CHECK(root.getFullPathName().endsWith("mwAudio/mw909/Presets"));
        CHECK(root.isAChildOf(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)));
    }
    auto p = makeProcessor();
    CHECK(p->presets().userRoot() == root);
    p->presets().setUserRoot(scratchDirectory("presets-location"));
    CHECK(p->presets().userDirectory(PresetKind::Kit).getFileName() == "Kits");
    CHECK(p->presets().userDirectory(PresetKind::Patterns).getFileName() == "Patterns");
    CHECK(p->presets().userDirectory(PresetKind::Full).getFileName() == "Full");
}

TEST_CASE("presets: factory content is listed first", "[plugin][presets]") {
    auto p = makeWithRoot("presets-factory");
    auto& presets = p->presets();

    CHECK(presets.numFactoryPresets(PresetKind::Kit) == mw909::factory::numKits());
    CHECK(presets.numFactoryPresets(PresetKind::Patterns) == mw909::factory::numBanks());
    CHECK(presets.numFactoryPresets(PresetKind::Full) == mw909::factory::numPresets());
    for (auto kind : {PresetKind::Kit, PresetKind::Patterns, PresetKind::Full}) {
        CHECK(presets.numPresets(kind) == presets.numFactoryPresets(kind));  // no user presets yet
        for (int i = 0; i < presets.numPresets(kind); ++i) {
            const PresetEntry entry = presets.preset(kind, i);
            CHECK(entry.factory);
            CHECK(entry.factoryIndex == i);
            CHECK(entry.name.isNotEmpty());
        }
        CHECK(presets.names(kind).size() == presets.numPresets(kind));
        CHECK(presets.preset(kind, -1).name.isEmpty());
        CHECK(presets.preset(kind, 100000).name.isEmpty());
        CHECK_FALSE(presets.load(kind, -1));
        CHECK_FALSE(presets.load(kind, 100000));
        CHECK_FALSE(presets.deleteUser(kind, 0));  // factory presets cannot be deleted
    }
    // A new instance is factory preset 0, and says so.
    CHECK(presets.currentName(PresetKind::Full) == presets.preset(PresetKind::Full, 0).name);
    CHECK(presets.currentIndex(PresetKind::Full) == 0);
}

TEST_CASE("presets: loading a factory kit sets its parameters inside gestures", "[plugin][presets]") {
    auto p = makeWithRoot("presets-kit-factory");
    auto& presets = p->presets();
    Rng rng(21U);
    const int kitIndex = presets.numFactoryPresets(PresetKind::Kit) - 1;

    randomiseParameters(*p, rng);
    const float tempoBefore = raw(*p, "tempo");
    const float muteBefore = raw(*p, "sd_mute");
    const float modeBefore = raw(*p, "seq_mode");
    const auto bankBefore = std::make_unique<mw909::seq::PatternBank>(p->patterns().bank());

    HostListener hostListener(*p);
    PresetListener listener;
    presets.addListener(&listener);
    REQUIRE(presets.load(PresetKind::Kit, kitIndex));

    for (const auto& [id, value] : mw909::plugin::Parameters::kitValues(mw909::factory::kit(kitIndex))) {
        auto& parameter = param(*p, juce::String(id));
        INFO(id);
        CHECK(raw(*p, juce::String(id)) == Approx(parameter.convertFrom0to1(parameter.convertTo0to1(value))).margin(1.0e-6));
    }
    CHECK(hostListener.gestureBegins > 0);
    CHECK(hostListener.gestureBegins == hostListener.gestureEnds);
    CHECK(hostListener.parameterChanges == hostListener.gestureBegins);

    // A kit is the sound and the mixer: nothing else moves.
    CHECK(raw(*p, "tempo") == tempoBefore);
    CHECK(raw(*p, "sd_mute") == muteBefore);
    CHECK(raw(*p, "seq_mode") == modeBefore);
    CHECK(sameBank(p->patterns().bank(), *bankBefore));
    CHECK(listener.loads == 1);
    CHECK(presets.currentName(PresetKind::Kit) == presets.preset(PresetKind::Kit, kitIndex).name);
    presets.removeListener(&listener);
}

TEST_CASE("presets: every factory kit, memory and full preset loads and plays", "[plugin][presets]") {
    auto p = makeWithRoot("presets-every-factory");
    auto& presets = p->presets();
    Host host(*p);
    setParam(*p, "seq_mode", 2.0f);  // Internal: the patterns play
    setParam(*p, "seq_run", 1.0f);
    for (auto kind : {PresetKind::Kit, PresetKind::Patterns, PresetKind::Full}) {
        for (int i = 0; i < presets.numFactoryPresets(kind); ++i) {
            INFO("kind " << static_cast<int>(kind) << " preset " << i << " " << presets.preset(kind, i).name);
            REQUIRE(presets.load(kind, i));
            CHECK(presets.currentIndex(kind) == i);
            host.run(9600, 512);
            CHECK(host.allFinite(512));
            CHECK(host.peak(0, 512) < 20.0f);
            if (kind == PresetKind::Patterns) {
                // A memory on its own brings its suggested tempo.
                CHECK(raw(*p, "tempo") == Approx(mw909::factory::bankInfo(i).tempo).margin(0.051));
                auto bank = std::make_unique<mw909::seq::PatternBank>();
                mw909::factory::buildBank(i, *bank);
                CHECK(sameBank(p->patterns().bank(), *bank));
            }
            if (kind == PresetKind::Full) {
                const auto& preset = mw909::factory::preset(i);
                for (const auto& [id, value] : mw909::plugin::Parameters::presetValues(preset)) {
                    auto& parameter = param(*p, juce::String(id));
                    CHECK(raw(*p, juce::String(id)) ==
                          Approx(parameter.convertFrom0to1(parameter.convertTo0to1(value))).margin(1.0e-6));
                }
                CHECK(p->patterns().selectedPattern() == preset.pattern);
                CHECK(p->getCurrentProgram() == i);
            }
        }
    }
    // The audio side ends up with the last memory loaded.
    host.process(64);
    CHECK(sameBank(p->machineForTests().sequencer().bank(), p->patterns().bank()));
}

TEST_CASE("presets: a user kit carries the user ROMs its voices play", "[plugin][presets][roms]") {
    auto p = makeWithRoot("presets-kit-roms");
    Rng rng(26U);
    const mw909::RomImage crash = randomRom(rng);
    const mw909::RomImage hat = randomRom(rng);
    REQUIRE(p->patterns().setUserRom(mw909::RomSlot::Crash, crash, "my crash"));
    REQUIRE(p->patterns().setUserRom(mw909::RomSlot::Hat, hat, "my hat"));
    setParam(*p, "cr_source", 2.0f);  // User
    setParam(*p, "ch_source", 1.0f);  // Synth: the hat slot is not used
    setParam(*p, "oh_source", 0.0f);
    REQUIRE(p->presets().saveUser(PresetKind::Kit, "With ROM"));
    {
        const auto xml = juce::parseXML(p->presets().userDirectory(PresetKind::Kit).getChildFile("With ROM.mw909"));
        REQUIRE(xml != nullptr);
        const auto* roms = xml->getChildByName("userRoms");
        REQUIRE(roms != nullptr);
        CHECK(roms->getNumChildElements() == 1);
        CHECK(roms->getFirstChildElement()->getIntAttribute("slot") == static_cast<int>(mw909::RomSlot::Crash));
    }

    auto q = makeProcessor();
    q->presets().setUserRoot(p->presets().userRoot());
    REQUIRE(q->presets().load(PresetKind::Kit, q->presets().numFactoryPresets(PresetKind::Kit)));
    const auto loaded = q->userRom(mw909::RomSlot::Crash);
    REQUIRE_FALSE(loaded.empty());
    CHECK(loaded.image->data == crash.data);
    CHECK(loaded.name == "my crash");
    CHECK(q->userRom(mw909::RomSlot::Hat).empty());
    CHECK(raw(*q, "cr_source") == 2.0f);

    // A kit without ROMs leaves the slots alone.
    REQUIRE(q->presets().load(PresetKind::Kit, 0));
    CHECK(q->userRom(mw909::RomSlot::Crash) == loaded);
}

TEST_CASE("presets: user kits, pattern banks and full presets save, load and delete", "[plugin][presets]") {
    auto p = makeWithRoot("presets-user");
    auto& presets = p->presets();
    Rng rng(22U);
    PresetListener listener;
    presets.addListener(&listener);

    // --- kit ---------------------------------------------------------------
    randomiseParameters(*p, rng);
    const auto kitState = p->captureState();
    REQUIRE(presets.saveUser(PresetKind::Kit, "My Kit"));
    const juce::File kitFile = presets.userDirectory(PresetKind::Kit).getChildFile("My Kit.mw909");
    CHECK(kitFile.existsAsFile());
    CHECK(kitFile.isAChildOf(juce::File(MW909_TEST_SCRATCH_DIR)));
    CHECK(presets.numPresets(PresetKind::Kit) == presets.numFactoryPresets(PresetKind::Kit) + 1);
    const int kitIndex = presets.numFactoryPresets(PresetKind::Kit);
    CHECK(presets.preset(PresetKind::Kit, kitIndex).name == "My Kit");
    CHECK_FALSE(presets.preset(PresetKind::Kit, kitIndex).factory);
    CHECK(presets.currentIndex(PresetKind::Kit) == kitIndex);
    CHECK(listener.listChanges > 0);

    // The file is the documented XML.
    {
        const auto xml = juce::parseXML(kitFile);
        REQUIRE(xml != nullptr);
        CHECK(xml->hasTagName("mw909preset"));
        CHECK(xml->getStringAttribute("kind") == "kit");
        CHECK(xml->getChildByName("PARAMETERS")->getNumChildElements() ==
              static_cast<int>(mw909::plugin::kitParameterIds().size()));
    }

    randomiseParameters(*p, rng);
    const auto beforeLoad = p->captureState();
    REQUIRE(presets.load(PresetKind::Kit, kitIndex));
    const std::set<std::string> kitIds(mw909::plugin::kitParameterIds().begin(), mw909::plugin::kitParameterIds().end());
    {
        const auto after = p->captureState();
        for (std::size_t i = 0; i < after->parameters.size(); ++i) {
            const bool inKit = kitIds.count(after->parameters[i].first.toStdString()) != 0;
            INFO(after->parameters[i].first);
            // Kit parameters come back exactly; the rest keep what they had.
            CHECK(after->parameters[i].second == (inKit ? kitState : beforeLoad)->parameters[i].second);
        }
    }

    // --- patterns ----------------------------------------------------------------
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    for (auto& pattern : bank->patterns) pattern = randomPattern(rng);
    bank->chain = randomChain(rng);
    for (auto& track : bank->tracks) track = randomSongTrack(rng);
    p->patterns().replaceBank(*bank);
    REQUIRE(presets.saveUser(PresetKind::Patterns, "My Bank"));
    p->patterns().replaceBank(*std::make_unique<mw909::seq::PatternBank>());
    const auto paramsBefore = p->captureState();
    const int bankIndex = presets.numFactoryPresets(PresetKind::Patterns);
    REQUIRE(presets.load(PresetKind::Patterns, bankIndex));
    CHECK(sameBank(p->patterns().bank(), *bank));
    CHECK(p->captureState()->parameters == paramsBefore->parameters);  // a bank moves no parameter
    {
        Host host(*p);
        host.process(64);
        CHECK(sameBank(p->machineForTests().sequencer().bank(), *bank));
    }

    // --- full ----------------------------------------------------------------------
    randomiseParameters(*p, rng);
    p->patterns().selectPattern(23);
    p->setUnitSeed(4711U);
    p->patterns().setUserRom(mw909::RomSlot::Ride, randomRom(rng), "ride");
    REQUIRE(presets.saveUser(PresetKind::Full, "Everything"));
    const auto fullState = p->captureState();

    randomiseParameters(*p, rng);
    p->patterns().replaceBank(*std::make_unique<mw909::seq::PatternBank>());
    p->patterns().selectPattern(2);
    p->setUnitSeed(1U);
    p->clearUserRom(mw909::RomSlot::Ride);
    p->midiLearn().assign(9, "bd_level");
    UiState ui = p->uiState();
    ui.scale = 1.5f;
    ui.view = 2;
    p->setUiState(ui);

    const int fullIndex = presets.numFactoryPresets(PresetKind::Full);
    REQUIRE(presets.load(PresetKind::Full, fullIndex));
    {
        const auto after = p->captureState();
        CHECK(after->parameters == fullState->parameters);
        CHECK(sameBank(after->model.bank, fullState->model.bank));
        CHECK(after->model.selectedPattern == 23);
        CHECK(after->model.unitSeed == 4711U);
        CHECK(after->model.roms == fullState->model.roms);
        // The session's own things stay: UI state and the MIDI-learn map.
        CHECK(after->model.ui.scale == 1.5f);
        CHECK(after->model.ui.view == 2);
        CHECK(p->midiLearn().controllerFor("bd_level") == 9);
        CHECK(presets.currentName(PresetKind::Full) == "Everything");
    }

    // --- stepping ---------------------------------------------------------------------
    {
        const int n = presets.numPresets(PresetKind::Kit);
        REQUIRE(presets.load(PresetKind::Kit, 0));
        for (int i = 1; i <= n; ++i) {
            REQUIRE(presets.loadNext(PresetKind::Kit));
            CHECK(presets.currentIndex(PresetKind::Kit) == i % n);  // wraps
        }
        REQUIRE(presets.loadPrevious(PresetKind::Kit));
        CHECK(presets.currentIndex(PresetKind::Kit) == n - 1);
    }

    // --- overwrite, names, delete -------------------------------------------------------
    REQUIRE(presets.saveUser(PresetKind::Kit, "My Kit"));  // same name: replaced, not duplicated
    CHECK(presets.numPresets(PresetKind::Kit) == presets.numFactoryPresets(PresetKind::Kit) + 1);
    CHECK_FALSE(presets.saveUser(PresetKind::Kit, ""));
    CHECK_FALSE(presets.saveUser(PresetKind::Kit, "   "));
    REQUIRE(presets.saveUser(PresetKind::Kit, "a/b\\c:d"));  // made legal, stays inside the directory
    CHECK(presets.numPresets(PresetKind::Kit) == presets.numFactoryPresets(PresetKind::Kit) + 2);
    for (const auto& file : presets.userDirectory(PresetKind::Kit).findChildFiles(juce::File::findFiles, true))
        CHECK(file.getParentDirectory() == presets.userDirectory(PresetKind::Kit));

    const int myKit = presets.names(PresetKind::Kit).indexOf("My Kit");
    REQUIRE(myKit >= presets.numFactoryPresets(PresetKind::Kit));
    REQUIRE(presets.deleteUser(PresetKind::Kit, myKit));
    CHECK_FALSE(kitFile.existsAsFile());
    CHECK(presets.numPresets(PresetKind::Kit) == presets.numFactoryPresets(PresetKind::Kit) + 1);
    CHECK(presets.names(PresetKind::Kit).indexOf("My Kit") == -1);
    CHECK_FALSE(presets.deleteUser(PresetKind::Kit, 1000));
    // The other kinds were not affected.
    CHECK(presets.userDirectory(PresetKind::Patterns).getChildFile("My Bank.mw909").existsAsFile());
    CHECK(presets.userDirectory(PresetKind::Full).getChildFile("Everything.mw909").existsAsFile());

    // A second instance pointed at the same directory sees the files.
    auto q = makeProcessor();
    q->presets().setUserRoot(presets.userRoot());
    CHECK(q->presets().numPresets(PresetKind::Full) == presets.numPresets(PresetKind::Full));
    REQUIRE(q->presets().load(PresetKind::Full, fullIndex));
    CHECK(q->captureState()->parameters == fullState->parameters);

    presets.removeListener(&listener);
}

TEST_CASE("presets: damaged preset files are refused and change nothing", "[plugin][presets]") {
    auto p = makeWithRoot("presets-damaged");
    auto& presets = p->presets();
    Rng rng(23U);
    randomiseParameters(*p, rng);
    const juce::File dir = presets.userDirectory(PresetKind::Kit);
    dir.createDirectory();

    dir.getChildFile("empty.mw909").replaceWithText("");
    dir.getChildFile("garbage.mw909").replaceWithText("\x01\x02 not xml at all <<<>>>");
    dir.getChildFile("foreign.mw909").replaceWithText("<?xml version=\"1.0\"?><other kind=\"kit\"/>");
    dir.getChildFile("nokind.mw909").replaceWithText("<mw909preset formatVersion=\"1\" kind=\"banana\"/>");
    dir.getChildFile("nothing.mw909").replaceWithText("<mw909preset formatVersion=\"1\" kind=\"kit\"/>");
    dir.getChildFile("truncated.mw909")
        .replaceWithText("<mw909preset kind=\"kit\"><PARAMETERS><PARAM id=\"bd_level\" value=\"0.");
    dir.getChildFile("fullbroken.mw909").replaceWithText("<mw909preset kind=\"full\"><mw909 stateVersion=\"77\"/></mw909preset>");
    dir.getChildFile("patternsbroken.mw909").replaceWithText("<mw909preset kind=\"patterns\"><sequencer patternFormat=\"9\"/></mw909preset>");
    dir.getChildFile("doctype.mw909")
        .replaceWithText("<?xml version=\"1.0\"?><!DOCTYPE mw909preset [<!ENTITY v \"0.25\">]>"
                         "<mw909preset kind=\"kit\"><PARAMETERS><PARAM id=\"bd_level\" value=\"&v;\"/>"
                         "</PARAMETERS></mw909preset>");
    {
        // Nested deep enough to exhaust the stack of the recursive XML reader.
        juce::MemoryOutputStream deep;
        deep << "<mw909preset kind=\"kit\"><PARAMETERS>";
        for (int i = 0; i < 300000; ++i) deep << "<x>";
        for (int i = 0; i < 300000; ++i) deep << "</x>";
        deep << "<PARAM id=\"bd_level\" value=\"0.25\"/></PARAMETERS></mw909preset>";
        dir.getChildFile("deep.mw909").replaceWithText(deep.toString());
    }
    // Not text at all (a UTF-16 byte-order mark, a NUL, then bytes no UTF-8 sequence allows).
    const unsigned char binary[] = {0xFF, 0xFE, 0x00, 'g', 'a', 'r', 'b', 'a', 'g', 'e', 0x80, 0x81, 0xC0, 0xAF};
    dir.getChildFile("binary.mw909").replaceWithData(binary, sizeof(binary));
    // Our own format behind a UTF-8 byte-order mark reads fine (checked below).
    presets.refresh();

    const juce::MemoryBlock before = saveState(*p);
    const int first = presets.numFactoryPresets(PresetKind::Kit);
    REQUIRE(presets.numPresets(PresetKind::Kit) == first + 11);
    for (int i = first; i < presets.numPresets(PresetKind::Kit); ++i) {
        INFO(presets.preset(PresetKind::Kit, i).name);
        CHECK_FALSE(presets.load(PresetKind::Kit, i));
        CHECK(saveState(*p) == before);
    }
    CHECK_FALSE(presets.loadFile(dir.getChildFile("does-not-exist.mw909")));

    // A kit file with odd values loads what it can, clamped.
    dir.getChildFile("odd.mw909")
        .replaceWithText("<mw909preset kind=\"kit\"><PARAMETERS>"
                         "<PARAM id=\"bd_level\" value=\"7\"/>"
                         "<PARAM id=\"sd_level\" value=\"nan\"/>"
                         "<PARAM id=\"tempo\" value=\"200\"/>"
                         "<PARAM id=\"bogus\" value=\"1\"/>"
                         "</PARAMETERS></mw909preset>");
    const float sdBefore = raw(*p, "sd_level");
    const float tempoBefore = raw(*p, "tempo");
    REQUIRE(presets.loadFile(dir.getChildFile("odd.mw909")));
    CHECK(raw(*p, "bd_level") == 1.0f);
    CHECK(raw(*p, "sd_level") == sdBefore);
    CHECK(raw(*p, "tempo") == tempoBefore);  // not a kit parameter

    // A UTF-8 byte-order mark in front of a valid file is tolerated.
    const juce::String bom = juce::String(juce::CharPointer_UTF8("\xEF\xBB\xBF"));
    dir.getChildFile("bom.mw909")
        .replaceWithText(bom + "<mw909preset kind=\"kit\"><PARAMETERS><PARAM id=\"bd_level\" value=\"0.25\"/>"
                               "</PARAMETERS></mw909preset>");
    REQUIRE(presets.loadFile(dir.getChildFile("bom.mw909")));
    CHECK(raw(*p, "bd_level") == 0.25f);
}

TEST_CASE("presets: host programs are the factory full presets", "[plugin][presets][programs]") {
    auto p = makeWithRoot("presets-programs");
    const int n = p->getNumPrograms();
    REQUIRE(n >= 1);
    CHECK(n == std::max(1, mw909::factory::numPresets()));
    CHECK(p->getCurrentProgram() == 0);
    for (int i = 0; i < n; ++i) CHECK(p->getProgramName(i).toStdString() == std::string(mw909::factory::preset(i).name));
    CHECK(p->getProgramName(-1).isEmpty());
    CHECK(p->getProgramName(n).isEmpty());
    p->changeProgramName(0, "ignored");
    CHECK(p->getProgramName(0).toStdString() == std::string(mw909::factory::preset(0).name));

    // Bad indices change nothing.
    Rng rng(24U);
    randomiseParameters(*p, rng);
    const juce::MemoryBlock tweaked = saveState(*p);
    p->setCurrentProgram(-1);
    p->setCurrentProgram(n);
    p->setCurrentProgram(1000000);
    CHECK(p->getCurrentProgram() == 0);
    CHECK(saveState(*p) == tweaked);
    // Re-selecting the current program reloads it (a host program menu, an
    // LV2 preset pick): the edits are gone.
    p->setCurrentProgram(0);
    CHECK(p->getCurrentProgram() == 0);
    {
        auto f = makeProcessor();
        const auto& preset0 = mw909::factory::preset(0);
        int differ = 0;
        for (const auto& [id, value] : mw909::plugin::Parameters::kitValues(mw909::factory::kit(preset0.kit)))
            if (raw(*p, juce::String(id)) != raw(*f, juce::String(id))) ++differ;
        for (const auto& [id, value] : mw909::plugin::Parameters::presetValues(preset0))
            if (raw(*p, juce::String(id)) != raw(*f, juce::String(id))) ++differ;
        CHECK(differ == 0);
        CHECK(sameBank(p->patterns().bank(), f->patterns().bank()));
    }
    loadState(*p, tweaked);
    CHECK(saveState(*p) == tweaked);

    if (n < 2) {
        WARN("only one factory preset: program switching not exercised");
        return;
    }

    // On the message thread a program loads at once: kit, bank, pattern, tempo.
    const int target = n - 1;
    const auto& preset = mw909::factory::preset(target);
    p->setCurrentProgram(target);
    CHECK(p->getCurrentProgram() == target);
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    mw909::factory::buildBank(preset.bank, *bank);
    CHECK(sameBank(p->patterns().bank(), *bank));
    CHECK(p->patterns().selectedPattern() == preset.pattern);
    CHECK(raw(*p, "tempo") == Approx(preset.tempo).margin(0.051));
    CHECK(p->presets().currentName(PresetKind::Full).toStdString() == std::string(preset.name));
    for (const auto& [id, value] : mw909::plugin::Parameters::kitValues(mw909::factory::kit(preset.kit))) {
        auto& parameter = param(*p, juce::String(id));
        CHECK(raw(*p, juce::String(id)) == Approx(parameter.convertFrom0to1(parameter.convertTo0to1(value))).margin(1.0e-6));
    }
    // ...and the audio thread has it with the next block.
    {
        Host host(*p);
        host.process(64);
        CHECK(sameBank(p->machineForTests().sequencer().bank(), *bank));
    }

    // The program is part of the saved state.
    auto q = makeProcessor();
    loadState(*q, saveState(*p));
    CHECK(q->getCurrentProgram() == target);

    // From another thread (the VST3 wrapper calls this on the audio thread)
    // nothing is touched: the request is parked for the message thread.
    const juce::MemoryBlock beforeOffThread = saveState(*p);
    allocspy::reset();
    std::thread other([&] {
        const allocspy::Scope realtime;
        p->setCurrentProgram(0);
    });
    other.join();
    CHECK(allocspy::allocations() == 0);
    CHECK(allocspy::deallocations() == 0);
    CHECK(p->getCurrentProgram() == 0);  // reported at once
    CHECK(sameBank(p->patterns().bank(), *bank));  // applied later
    p->serviceMessageThread();
    mw909::factory::buildBank(mw909::factory::preset(0).bank, *bank);
    CHECK(sameBank(p->patterns().bank(), *bank));
    CHECK(saveState(*p) != beforeOffThread);

    // A state load after a parked request wins over it.
    std::thread again([&] { p->setCurrentProgram(target); });
    again.join();
    loadState(*p, tweaked);
    p->serviceMessageThread();
    CHECK(saveState(*p) == tweaked);

    // A program change parked by the audio thread and a save on a worker
    // thread before the timer ran: the saved state names the program its
    // parameters belong to (the one applied last), never the parked one.
    {
        auto r = makeProcessor();
        std::thread audio([&] { r->setCurrentProgram(target); });
        audio.join();
        CHECK(r->getCurrentProgram() == target);
        juce::MemoryBlock saved;
        std::thread worker([&] { r->getStateInformation(saved); });
        worker.join();
        auto loaded = makeProcessor();
        loadState(*loaded, saved);
        loaded->serviceMessageThread();
        CHECK(loaded->getCurrentProgram() == 0);
        CHECK(saveState(*loaded) == saveState(*makeProcessor()));
        // The parked change still happens on the original.
        r->serviceMessageThread();
        CHECK(r->getCurrentProgram() == target);
        auto s = makeProcessor();
        s->setCurrentProgram(target);
        CHECK(saveState(*r) == saveState(*s));
        // ...and from then on a save names it.
        std::thread worker2([&] { r->getStateInformation(saved); });
        worker2.join();
        auto t = makeProcessor();
        loadState(*t, saved);
        CHECK(t->getCurrentProgram() == target);
        CHECK(saveState(*t) == saveState(*s));
    }

    // Loading a factory full preset from the preset manager moves the host's
    // program too, and tells it.
    HostListener hostListener(*p);
    REQUIRE(p->presets().load(PresetKind::Full, target));
    CHECK(p->getCurrentProgram() == target);
    p->serviceMessageThread();
    CHECK(hostListener.programChanges == 1);
}

TEST_CASE("midi files: a pattern exports to a temp file and imports back exactly", "[plugin][midifile]") {
    auto p = makeProcessor();
    const juce::File dir = scratchDirectory("midi-export");
    p->setMidiExportDirectory(dir);
    Rng rng(25U);

    mw909::seq::Pattern pattern;
    pattern.shuffle = 10;
    for (int s = 0; s < 16; s += 4) pattern.at(0, mw909::BD, s).setLevel(mw909::seq::StepLevel::Normal);
    pattern.at(0, mw909::SD, 4).setLevel(mw909::seq::StepLevel::Accent);
    pattern.at(0, mw909::CH, 3).setProbabilityCode(3);
    pattern.at(0, mw909::CH, 3).setLevel(mw909::seq::StepLevel::Weak);
    pattern.at(1, mw909::RD, 7).setLevel(mw909::seq::StepLevel::Normal);
    p->patterns().setPattern(18, pattern);

    const juce::File file = p->exportPatternToMidiFile(18, 0);
    REQUIRE(file.existsAsFile());
    CHECK(file.getParentDirectory() == dir);
    CHECK(file.hasFileExtension(".mid"));
    CHECK(file.getFileName().contains("I-2-03"));  // bank I, group 2, pattern 3
    juce::MemoryBlock head;
    REQUIRE(file.loadFileAsData(head));
    REQUIRE(head.getSize() > 14);
    CHECK(std::memcmp(head.getData(), "MThd", 4) == 0);

    CHECK(p->exportPatternToMidiFile(-1, 0) == juce::File());
    CHECK(p->exportPatternToMidiFile(96, 0) == juce::File());
    CHECK(p->exportPatternToMidiFile(95, 1).getFileName().contains("II-3-16 B"));
    CHECK(p->exportPatternToMidiFile(18, 7).existsAsFile());  // variation clamped

    // Import into another slot: the file carries variation A exactly (length,
    // scale, shuffle, every step bit); the slot's other variation is kept.
    const mw909::seq::Pattern slotBefore = p->patterns().pattern(40);
    REQUIRE(p->importMidiFile(file, 40));
    const mw909::seq::Pattern imported = p->patterns().pattern(40);
    CHECK(imported.length == pattern.length);
    CHECK(imported.scale == pattern.scale);
    CHECK(imported.shuffle == pattern.shuffle);
    CHECK(imported.trackLength == pattern.trackLength);
    CHECK(imported.steps[0] == pattern.steps[0]);
    CHECK(imported.steps[1] == slotBefore.steps[1]);
    CHECK(imported.at(0, mw909::CH, 3).probabilityCode() == 3);
    Host host(*p);
    host.process(64);
    CHECK(p->machineForTests().sequencer().bank().patterns[40] == imported);

    // Variation B goes out and comes back the same way, into variation B.
    const juce::File fileB = p->exportPatternToMidiFile(18, 1);
    REQUIRE(fileB.existsAsFile());
    CHECK(fileB != file);
    REQUIRE(p->importMidiFile(fileB, 40, 1));
    CHECK(p->patterns().pattern(40).steps[1] == pattern.steps[1]);
    CHECK(p->patterns().pattern(40).steps[0] == pattern.steps[0]);

    // Refusals leave the model alone.
    const mw909::seq::Pattern before = p->patterns().pattern(41);
    CHECK_FALSE(p->importMidiFile(dir.getChildFile("missing.mid"), 41));
    dir.getChildFile("junk.mid").replaceWithText("this is not a MIDI file");
    CHECK_FALSE(p->importMidiFile(dir.getChildFile("junk.mid"), 41));
    dir.getChildFile("empty.mid").replaceWithText("");
    CHECK_FALSE(p->importMidiFile(dir.getChildFile("empty.mid"), 41));
    CHECK_FALSE(p->importMidiFile(file, -1));
    CHECK_FALSE(p->importMidiFile(file, 96));
    CHECK(p->patterns().pattern(41) == before);
}

TEST_CASE("editor: the placeholder opens and closes", "[plugin][editor]") {
    auto p = makeProcessor();
    REQUIRE(p->hasEditor());
    for (int i = 0; i < 3; ++i) {
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        REQUIRE(editor != nullptr);
        CHECK(editor->getWidth() > 0);
        CHECK(editor->getHeight() > 0);
        editor->setSize(700, 500);
        // The plugin keeps working with an editor around.
        Host host(*p);
        host.process(256);
        p->serviceMessageThread();
        CHECK(host.allFinite(256));
    }
}
