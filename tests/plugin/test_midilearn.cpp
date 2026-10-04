// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// MIDI learn (docs/design/plugin.md §5): controllers travel from the audio
// thread through the FIFO and are applied on the message thread.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"

using namespace plugintest;
using Catch::Approx;

namespace {

struct LearnListener final : mw909::plugin::MidiLearn::Listener {
    int changes = 0;
    void midiLearnChanged() override { ++changes; }
};

void sendCc(Host& host, int controller, int value, int channel = 1) {
    host.midi.clear();
    host.cc(controller, value, 0, channel);
    host.process(64);
    host.midi.clear();
}

}  // namespace

TEST_CASE("midi learn: learn, control, forget", "[plugin][midilearn]") {
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    auto& learn = p->midiLearn();
    LearnListener listener;
    learn.addListener(&listener);
    HostListener hostListener(*p);

    // A fresh instance has the factory CC map; start from an empty one here.
    CHECK(learn.getMap() == mw909::plugin::MidiLearn::factoryMap());
    learn.clear();
    CHECK(learn.getMap().empty());
    p->serviceMessageThread();  // reports the cleared map to the host
    listener.changes = 0;
    hostListener.stateChanges = 0;
    CHECK_FALSE(learn.isLearning());
    CHECK(learn.controllerFor("bd_decay") == -1);

    // A controller that arrives before learning is armed means nothing.
    sendCc(host, 20, 100);
    p->serviceMessageThread();
    CHECK(learn.getMap().empty());

    learn.beginLearn("bd_decay");
    CHECK(learn.isLearning());
    CHECK(learn.learningParameter() == "bd_decay");
    CHECK(listener.changes == 1);

    const float before = raw(*p, "bd_decay");
    sendCc(host, 74, 127, 5);  // any channel
    // The audio thread only queued it: nothing has changed yet.
    CHECK(raw(*p, "bd_decay") == before);
    CHECK(learn.isLearning());

    p->serviceMessageThread();
    CHECK_FALSE(learn.isLearning());
    CHECK(learn.learningParameter().isEmpty());
    CHECK(learn.controllerFor("bd_decay") == 74);
    CHECK(learn.parameterFor(74) == "bd_decay");
    CHECK(raw(*p, "bd_decay") == 1.0f);  // the learning move already sets the value
    CHECK(listener.changes == 2);
    CHECK(hostListener.gestureBegins == 1);
    CHECK(hostListener.gestureEnds == 1);
    CHECK(hostListener.stateChanges == 1);  // the map is part of the session

    // From now on the controller drives the parameter.
    sendCc(host, 74, 0);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_decay") == 0.0f);
    sendCc(host, 74, 64);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_decay") == Approx(64.0f / 127.0f).margin(1.0e-6));

    // Several values in one block: the last one wins, as one change.
    const int gestures = hostListener.gestureBegins;
    host.midi.clear();
    host.cc(74, 10, 0);
    host.cc(74, 20, 5);
    host.cc(74, 30, 9);
    host.process(64);
    host.midi.clear();
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_decay") == Approx(30.0f / 127.0f).margin(1.0e-6));
    CHECK(hostListener.gestureBegins == gestures + 1);

    // Other controllers do nothing.
    sendCc(host, 75, 127);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_decay") == Approx(30.0f / 127.0f).margin(1.0e-6));

    learn.forget("bd_decay");
    CHECK(learn.controllerFor("bd_decay") == -1);
    CHECK(learn.getMap().empty());
    sendCc(host, 74, 127);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_decay") == Approx(30.0f / 127.0f).margin(1.0e-6));

    learn.removeListener(&listener);
}

TEST_CASE("midi learn: binding rules", "[plugin][midilearn]") {
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    auto& learn = p->midiLearn();
    learn.clear();

    CHECK(learn.assign(10, "sd_level"));
    CHECK(learn.assign(11, "bd_mute"));
    CHECK(learn.assign(12, "bd_out"));
    CHECK(learn.assign(13, "tempo"));
    CHECK_FALSE(learn.assign(14, "no_such_parameter"));
    CHECK_FALSE(learn.assign(-1, "sd_level"));
    CHECK_FALSE(learn.assign(120, "sd_level"));  // channel-mode controllers
    CHECK_FALSE(learn.assign(128, "sd_level"));

    // One controller per parameter: re-binding moves it.
    CHECK(learn.assign(15, "sd_level"));
    CHECK(learn.controllerFor("sd_level") == 15);
    CHECK(learn.parameterFor(10).isEmpty());
    // One parameter per controller: the newer binding replaces the older.
    CHECK(learn.assign(15, "ch_level"));
    CHECK(learn.controllerFor("sd_level") == -1);
    CHECK(learn.parameterFor(15) == "ch_level");

    // Switches, choices and stepped ranges get values they can hold.
    sendCc(host, 11, 100);
    sendCc(host, 12, 127);
    sendCc(host, 13, 64);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_mute") == 1.0f);
    CHECK(param(*p, "bd_mute").getValue() == 1.0f);
    CHECK(raw(*p, "bd_out") == 15.0f);
    const float tempo = raw(*p, "tempo");
    CHECK(tempo == Approx(20.0 + 380.0 * 64.0 / 127.0).margin(0.051));
    CHECK(std::abs(tempo * 10.0f - std::round(tempo * 10.0f)) < 1.0e-3f);  // on the 0.1 BPM grid
    sendCc(host, 11, 20);
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_mute") == 0.0f);

    // cancelLearn leaves the map alone.
    learn.beginLearn("cr_color");
    learn.cancelLearn();
    CHECK_FALSE(learn.isLearning());
    sendCc(host, 50, 60);
    p->serviceMessageThread();
    CHECK(learn.controllerFor("cr_color") == -1);
    learn.beginLearn("no_such_parameter");
    CHECK_FALSE(learn.isLearning());

    learn.forgetController(11);
    CHECK(learn.controllerFor("bd_mute") == -1);
    CHECK(learn.getMap().size() == 3U);
    learn.clear();
    CHECK(learn.getMap().empty());
}

TEST_CASE("midi learn: the map is saved with the session", "[plugin][midilearn]") {
    auto p = makeProcessor();
    p->midiLearn().clear();
    p->midiLearn().assign(1, "master_volume");
    p->midiLearn().assign(7, "bd_level");
    p->midiLearn().assign(119, "ch_decay");
    const auto saved = saveState(*p);

    auto q = makeProcessor();
    q->midiLearn().assign(33, "cr_color");  // replaced by the load
    LearnListener listener;
    q->midiLearn().addListener(&listener);
    loadState(*q, saved);
    CHECK(listener.changes == 1);
    const auto map = q->midiLearn().getMap();
    REQUIRE(map.size() == 3U);
    CHECK(map[0] == std::pair<int, juce::String>{1, "master_volume"});
    CHECK(map[1] == std::pair<int, juce::String>{7, "bd_level"});
    CHECK(map[2] == std::pair<int, juce::String>{119, "ch_decay"});

    // And it works right away.
    Host host(*q);
    host.head.playing = false;
    sendCc(host, 7, 0);
    q->serviceMessageThread();
    CHECK(raw(*q, "bd_level") == 0.0f);
    q->midiLearn().removeListener(&listener);
}

TEST_CASE("midi learn: the factory CC map drives the panel with no learning", "[plugin][midilearn]") {
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    auto& learn = p->midiLearn();
    const auto& map = mw909::plugin::MidiLearn::factoryMap();

    // plugin.md §5: the original machine's family map.
    const std::vector<std::pair<int, const char*>> expected = {
        {9, "swing_scale"}, {20, "bd_tune"}, {21, "bd_attack"}, {22, "bd_ch_drive"}, {23, "bd_decay"},
        {24, "bd_level"},   {25, "sd_tune"}, {26, "sd_snappy"}, {27, "sd_comp_on"},  {28, "sd_tone"},
        {29, "sd_level"},   {46, "lt_tune"}, {47, "lt_decay"},  {48, "lt_level"},    {49, "mt_tune"},
        {50, "mt_decay"},   {51, "mt_level"}, {52, "ht_tune"},  {53, "ht_decay"},    {54, "ht_level"},
        {55, "rs_tune"},    {56, "rs_decay"}, {57, "rs_level"}, {58, "cp_tune"},     {59, "cp_decay"},
        {60, "cp_level"},   {61, "ch_tune"}, {62, "ch_decay"},  {63, "ch_level"},    {71, "total_accent"},
        {80, "oh_tune"},    {81, "oh_decay"}, {82, "oh_level"}, {83, "cr_tune"},     {84, "cr_decay"},
        {85, "cr_level"},   {86, "rd_tune"}, {87, "rd_decay"},  {88, "rd_level"},
    };
    REQUIRE(map.size() == expected.size());
    for (std::size_t i = 0; i < map.size(); ++i) {
        CHECK(map[i].first == expected[i].first);
        CHECK(map[i].second == expected[i].second);
        CHECK(p->apvts().getParameter(map[i].second) != nullptr);
        CHECK(learn.controllerFor(map[i].second) == map[i].first);
    }

    sendCc(host, 71, 0);
    sendCc(host, 85, 127);
    sendCc(host, 9, 64);
    p->serviceMessageThread();
    CHECK(raw(*p, "total_accent") == 0.0f);
    CHECK(raw(*p, "cr_level") == 1.0f);
    CHECK(raw(*p, "swing_scale") == Approx(64.0f / 127.0f).margin(1.0e-6));

    // Learn overrides one CC of it; forget and reset work per controller.
    learn.beginLearn("fx_reverb_mix");
    sendCc(host, 71, 10);
    p->serviceMessageThread();
    CHECK(learn.parameterFor(71) == "fx_reverb_mix");
    CHECK(learn.controllerFor("total_accent") == -1);
    learn.forgetController(85);
    CHECK(learn.parameterFor(85).isEmpty());
    CHECK(learn.parameterFor(86) == "rd_tune");
    learn.resetToFactory();
    CHECK(learn.getMap() == map);

    // The audio thread passes a CC on only when it is bound (or a learn is armed).
    learn.clear();
    sendCc(host, 24, 0);
    const float level = raw(*p, "bd_level");
    p->serviceMessageThread();
    CHECK(raw(*p, "bd_level") == level);
}
