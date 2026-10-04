// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// mw909_ui_tool: drives the real mw909Editor headlessly (see CMakeLists.txt).
// Exit code 0 on success, 1 on a failed check, 2 on bad usage.

#include <sys/resource.h>
#include <unistd.h>

#if JUCE_LINUX || defined(__linux__)
 #include <X11/Xlib.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <typeinfo>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "FxView.h"
#include "GridView.h"
#include "HeaderBar.h"
#include "MixerView.h"
#include "SequencerStrip.h"
#include "UiCommon.h"
#include "VoicePanel.h"
#include "mw909/MidiMap.h"
#include "mw909/seq/PatternOps.h"
#include "mw909Editor.h"
#include "mw909Processor.h"

namespace {

using mw909::ui::View;
namespace paramid = mw909::plugin::paramid;

int failures = 0;

// ASan / TSan builds: CPU figures there measure the instrumentation more than
// the editor, so the CPU budget becomes a runaway guard (see idle()).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizerBuild = true;
#else
constexpr bool kSanitizerBuild = false;
#endif

// Under Xvfb (no window manager) a plain titled window is what pluginval and
// the Standalone use; "temporary" windows need WM atoms that may not exist.
constexpr int kWindowFlags = juce::ComponentPeer::windowHasTitleBar | juce::ComponentPeer::windowIsResizable;
constexpr const char* kViewFileNames[mw909::ui::kNumViews] = {"panel", "grid", "mixer", "fx"};

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// JUCE reads its X connection when poll() reports the socket readable, but
// while a shared-memory paint is in flight it calls XCheckTypedWindowEvent on
// every vblank, and that moves everything the server has sent so far into
// Xlib's queue. An event that arrives in that window (on a freshly mapped
// editor: its ConfigureNotify, MapNotify and full-window Expose) then waits in
// the queue, unseen, until some later event makes the socket readable again.
// On a bare Xvfb nothing else sends one, and the stale Expose surfaced up to
// 20 s later as a full-window repaint in the middle of a perf measurement
// (under the sanitizers, where the first paints are slow enough to hit that
// window every time). A no-op ClientMessage sent to the window from a second
// connection wakes JUCE's poll, and its handler drains the whole queue.
void wakeWindowEvents(juce::ComponentPeer* peer) {
#if defined(__linux__)
    struct Connection {
        Display* display = XOpenDisplay(nullptr);
        ~Connection() {
            if (display != nullptr) XCloseDisplay(display);
        }
    };
    static Connection connection;
    if (connection.display == nullptr || peer == nullptr) return;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(peer->getNativeHandle()));
    event.xclient.message_type = XInternAtom(connection.display, "MW909_UI_TOOL_WAKE", False);
    event.xclient.format = 32;
    // No event mask: delivered to the client that created the window (JUCE).
    XSendEvent(connection.display, event.xclient.window, False, NoEventMask, &event);
    XFlush(connection.display);
#else
    juce::ignoreUnused(peer);
#endif
}

// Let the editor settle for `ms` before a measurement. The wake goes out near
// the end, when the editor has finished painting (no paint in flight to
// swallow it), so window events still queued from earlier are handled here
// rather than counted in the measurement.
void settle(juce::ComponentPeer* peer, int ms) {
    pump(juce::jmax(0, ms - 100));
    wakeWindowEvents(peer);
    pump(juce::jmin(ms, 100));
}

double cpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1.0e-6;
}

juce::File scratchDir() {
    auto d = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mw909_ui_tool");
    d.createDirectory();
    return d;
}

// --- a host stand-in running processBlock on its own thread ----------------------------------

class AudioThread {
public:
    explicit AudioThread(mw909Processor& p, bool play) : processor_(p), play_(play) {
        buffer_.setSize(juce::jmax(2, processor_.getTotalNumOutputChannels()), kBlock);
        thread_ = std::thread([this] { run(); });
    }
    ~AudioThread() {
        stop_ = true;
        thread_.join();
    }
    long blocks() const noexcept { return blocks_.load(); }

private:
    static constexpr int kBlock = 256;
    struct PlayHead final : juce::AudioPlayHead {
        std::atomic<double> ppq{0.0};
        std::atomic<bool> playing{false};
        juce::Optional<PositionInfo> getPosition() const override {
            PositionInfo info;
            info.setIsPlaying(playing.load());
            info.setPpqPosition(ppq.load());
            info.setBpm(124.0);
            return info;
        }
    };
    void run() {
        processor_.setPlayHead(&playHead_);
        playHead_.playing = play_;
        juce::Random rng(909);
        while (!stop_) {
            midi_.clear();
            if (play_ && rng.nextInt(8) == 0) {
                const int v = rng.nextInt(mw909::kNumVoices);
                midi_.addEvent(juce::MidiMessage::noteOn(10, mw909::kVoiceNote[static_cast<std::size_t>(v)],
                                                         static_cast<juce::uint8>(60 + rng.nextInt(67))),
                               0);
            }
            processor_.processBlock(buffer_, midi_);
            ++blocks_;
            playHead_.ppq = playHead_.ppq.load() + 124.0 / 60.0 * kBlock / 48000.0;
            std::this_thread::sleep_for(std::chrono::microseconds(5300));  // ~ real time at 48 kHz
        }
        processor_.setPlayHead(nullptr);
    }
    mw909Processor& processor_;
    bool play_;
    PlayHead playHead_;
    juce::AudioBuffer<float> buffer_;
    juce::MidiBuffer midi_;
    std::atomic<bool> stop_{false};
    std::atomic<long> blocks_{0};
    std::thread thread_;
};

std::unique_ptr<mw909Processor> makeProcessor() {
    auto p = std::make_unique<mw909Processor>();
    p->presets().setUserRoot(scratchDir().getChildFile("presets"));
    p->prepareToPlay(48000.0, 256);
    return p;
}

void setParam(mw909Processor& p, const juce::String& id, float value) {
    if (auto* param = p.apvts().getParameter(id)) param->setValueNotifyingHost(param->convertTo0to1(value));
}

float raw(mw909Processor& p, const juce::String& id) { return p.apvts().getRawParameterValue(id)->load(); }

// Give the screenshots something to show: modifiers, accents and a polymeter lane.
void decoratePattern(mw909Processor& p) {
    auto& model = p.patterns();
    model.edit(model.selectedPattern(), [](mw909::seq::Pattern& pat) {
        using mw909::seq::StepLevel;
        for (int s = 0; s < 16; s += 4) pat.at(0, mw909::BD, s).setLevel(StepLevel::Accent);
        pat.at(0, mw909::BD, 14).setLevel(StepLevel::Weak);
        auto& sd = pat.at(0, mw909::SD, 4);
        sd.setLevel(StepLevel::Accent);
        sd.setFlam(true);
        pat.at(0, mw909::SD, 12).setLevel(StepLevel::Normal);
        auto& sd2 = pat.at(0, mw909::SD, 15);
        sd2.setLevel(StepLevel::Weak);
        sd2.setSubSteps(3);
        for (int s = 2; s < 16; s += 4) pat.at(0, mw909::OH, s).setLevel(StepLevel::Normal);
        for (int s = 0; s < 3; ++s) pat.at(0, mw909::CH, s).setLevel(s == 0 ? StepLevel::Accent : StepLevel::Normal);
        pat.trackLength[mw909::CH] = 3;
        auto& cp = pat.at(0, mw909::CP, 12);
        cp.setLevel(StepLevel::Weak);
        cp.setProbabilityCode(2);
        pat.at(0, mw909::RD, 8).setLevel(StepLevel::Normal);
        pat.at(0, mw909::LT, 10).setLevel(StepLevel::Normal);
        pat.at(0, mw909::MT, 11).setLevel(StepLevel::Normal);
        pat.at(0, mw909::HT, 13).setLevel(StepLevel::Accent);
        pat.at(0, mw909::seq::kAccentTrack, 0).setLevel(StepLevel::Accent);
        pat.at(0, mw909::seq::kAccentTrack, 8).setLevel(StepLevel::Accent);
    });
}

// Run the processor on this thread for a few blocks (the internal clock, auditions).
void advanceTransport(mw909Processor& p, int blocks) {
    juce::AudioBuffer<float> buffer(juce::jmax(2, p.getTotalNumOutputChannels()), 256);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i) p.processBlock(buffer, midi);
}

bool savePng(const juce::Image& image, const juce::File& file) {
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(image, out);
}

juce::File writeTestWav(const juce::File& file, double rate, double seconds) {
    file.deleteFile();
    const int n = juce::jmax(1, static_cast<int>(rate * seconds));
    juce::AudioBuffer<float> audio(1, n);
    juce::Random rng(7);
    for (int i = 0; i < n; ++i)
        audio.setSample(0, i, (rng.nextFloat() * 2.0f - 1.0f) * 0.4f * std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(n)));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(16));
    if (writer == nullptr) return {};
    writer->writeFromAudioSampleBuffer(audio, 0, n);
    return file;
}

// --- component helpers -----------------------------------------------------------------------

template <class T>
void collect(juce::Component& root, juce::Array<T*>& out) {
    if (auto* t = dynamic_cast<T*>(&root)) out.add(t);
    for (auto* child : root.getChildren()) collect<T>(*child, out);
}

template <class T = juce::TextButton>
T* findButton(juce::Component& root, const juce::String& text, juce::Component* within = nullptr) {
    juce::Array<T*> all;
    collect<T>(root, all);
    for (auto* b : all)
        if (b->getButtonText() == text && b->isShowing() && (within == nullptr || within->isParentOf(b))) return b;
    return nullptr;
}

template <class T>
T* findOne(juce::Component& root) {
    juce::Array<T*> all;
    collect<T>(root, all);
    for (auto* c : all)
        if (c->isShowing()) return c;
    return all.isEmpty() ? nullptr : all.getFirst();
}

mw909::ui::ParamControl* findControl(juce::Component& root, const juce::String& id) {
    juce::Array<mw909::ui::ParamControl*> all;
    collect(root, all);
    for (auto* c : all)
        if (c->parameter().getParameterID() == id && c->isShowing()) return c;
    return nullptr;
}

template <class T>
T* findBound(juce::Component& root, const juce::String& id) {
    juce::Array<T*> all;
    collect(root, all);
    for (auto* c : all)
        if (c->parameter().getParameterID() == id && c->isShowing()) return c;
    return nullptr;
}

// Live (not zombie) native file-dialog processes this process started: what
// a juce::FileChooser runs on Linux (zenity or kdialog).
int liveDialogChildren() {
    int n = 0;
    const int self = static_cast<int>(getpid());
    for (const auto& entry : juce::RangedDirectoryIterator(juce::File("/proc"), false, "*", juce::File::findDirectories)) {
        const juce::String stat = entry.getFile().getChildFile("stat").loadFileAsString();
        const int open = stat.indexOfChar('('), close = stat.lastIndexOfChar(')');
        if (open < 0 || close < open) continue;
        const juce::String comm = stat.substring(open + 1, close);
        const auto rest = juce::StringArray::fromTokens(stat.substring(close + 2), " ", "");
        if (rest.size() < 2 || rest[1].getIntValue() != self || rest[0] == "Z") continue;
        if (comm == "zenity" || comm == "kdialog") ++n;
    }
    return n;
}

bool dialogToolInstalled() {
    for (const char* tool : {"/usr/bin/zenity", "/usr/bin/kdialog", "/usr/local/bin/zenity", "/usr/local/bin/kdialog"})
        if (juce::File(tool).existsAsFile()) return true;
    return false;
}

// The topmost modal component (an open popup menu or alert), or nullptr.
juce::Component* topModal() {
    auto* mcm = juce::ModalComponentManager::getInstanceWithoutCreating();
    return mcm != nullptr && mcm->getNumModalComponents() > 0 ? mcm->getModalComponent(0) : nullptr;
}

// Pick an item of the open popup menu by keyboard: `downs` x down, optional
// right (into a submenu) + `subDowns` x down, then return.
bool chooseMenuItem(int downs, int subDowns = -1) {
    pump(40);
    auto* menu = topModal();
    if (menu == nullptr) return false;
    for (int i = 0; i < downs; ++i) menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    if (subDowns >= 0) {
        menu->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        pump(40);
        auto* sub = topModal();
        if (sub == nullptr || sub == menu) return false;
        // Opening a submenu by key highlights its first item already.
        for (int i = 0; i < subDowns; ++i) sub->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        menu = sub;
    }
    menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump(60);
    return true;
}

struct Mouse {
    juce::ComponentPeer* peer;
    juce::Point<float> at(juce::Component& c, float fx = 0.5f, float fy = 0.5f) const {
        const auto screen = c.localPointToGlobal(juce::Point<float>(static_cast<float>(c.getWidth()) * fx, static_cast<float>(c.getHeight()) * fy));
        return peer->globalToLocal(screen);
    }
    juce::Point<float> at(juce::Component& c, juce::Point<int> local) const {
        return peer->globalToLocal(c.localPointToGlobal(local.toFloat()));
    }
    void move(juce::Point<float> p, juce::ModifierKeys mods = {}) const {
        peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, p, mods, 0.0f, 0.0f, juce::Time::currentTimeMillis());
        pump(5);
    }
    // The key modifiers of a synthetic event come from ModifierKeys::currentModifiers
    // (the X11 backend sets it from key events), so set it explicitly here.
    void press(juce::Point<float> p, juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier) const {
        juce::ModifierKeys::currentModifiers = mods.withoutMouseButtons();
        move(p, mods.withoutMouseButtons());
        peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, p, mods, 0.0f, 0.0f, juce::Time::currentTimeMillis());
        pump(5);
    }
    void release(juce::Point<float> p, juce::ModifierKeys mods = {}) const {
        peer->handleMouseEvent(juce::MouseInputSource::InputSourceType::mouse, p, mods.withoutMouseButtons(), 0.0f, 0.0f,
                               juce::Time::currentTimeMillis());
        juce::ModifierKeys::currentModifiers = {};
        pump(5);
    }
    void clickAt(juce::Point<float> p, juce::ModifierKeys extra = {}) const {
        press(p, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier).withFlags(extra.getRawFlags()));
        release(p, extra);
    }
    void click(juce::Component& c, juce::ModifierKeys extra = {}) const { clickAt(at(c), extra); }
    void rightClick(juce::Component& c) const {
        const auto p = at(c);
        press(p, juce::ModifierKeys::rightButtonModifier);
        release(p);
    }
    void doubleClick(juce::Component& c) const {
        click(c);
        click(c);
    }
    void drag(juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys extra = {}, int steps = 6) const {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier).withFlags(extra.getRawFlags());
        press(from, mods);
        for (int i = 1; i <= steps; ++i) move(from + (to - from) * (static_cast<float>(i) / static_cast<float>(steps)), mods);
        release(to, extra);
    }
    void dragVertical(juce::Component& c, int dy, juce::ModifierKeys extra = {}) const {
        // A knob is grabbed by its cap (its value may sit beside it).
        const auto* knob = dynamic_cast<mw909::ui::ParamControl*>(&c);
        const auto p = knob != nullptr ? at(c, knob->controlBounds().getCentre()) : at(c);
        drag(p, p.translated(0.0f, static_cast<float>(dy)), extra, 4);
    }
    void wheel(juce::Component& c, float deltaY) const {
        const auto p = at(c);
        move(p);
        juce::MouseWheelDetails d;
        d.deltaX = 0.0f;
        d.deltaY = deltaY;
        d.isReversed = false;
        d.isSmooth = false;
        d.isInertial = false;
        peer->handleMouseWheel(juce::MouseInputSource::InputSourceType::mouse, p, juce::Time::currentTimeMillis(), d);
        pump(5);
    }
};

struct OpenEditor {
    std::unique_ptr<juce::AudioProcessorEditor> base;
    mw909Editor* editor = nullptr;
    juce::ComponentPeer* peer = nullptr;
    explicit OpenEditor(mw909Processor& p, float scale = 1.0f) {
        base.reset(p.createEditor());
        editor = dynamic_cast<mw909Editor*>(base.get());
        if (editor == nullptr) return;
        editor->setScale(scale);
        editor->addToDesktop(kWindowFlags);
        editor->setVisible(true);
        pump(100);
        peer = editor->getPeer();
    }
    ~OpenEditor() {
        if (editor != nullptr) editor->removeFromDesktop();
    }
};

// --- screenshots -----------------------------------------------------------------------------

int screenshots(const juce::File& dir) {
    dir.createDirectory();
    auto processor = makeProcessor();
    setParam(*processor, paramid::seqMode, 2.0f);  // Internal
    setParam(*processor, paramid::seqRun, 1.0f);
    decoratePattern(*processor);
    processor->serviceMessageThread();
    advanceTransport(*processor, 40);  // ~ 5 steps at 120 BPM
    processor->serviceMessageThread();

    OpenEditor open(*processor);
    check(open.editor != nullptr, "createEditor() returns an mw909Editor");
    if (open.editor == nullptr || open.peer == nullptr) return 1;
    auto& ed = *open.editor;
    Mouse mouse{open.peer};

    auto shoot = [&](const juce::String& name) {
        // Let a few hits land between two polls so the trigger lamps and meters show.
        ed.pollNow();
        advanceTransport(*processor, 6);
        processor->serviceMessageThread();
        ed.pollNow();
        pump(40);
        const juce::Image image = ed.createComponentSnapshot(ed.getLocalBounds(), true, 1.0f);
        const juce::File file = dir.getChildFile(name + ".png");
        const bool ok = savePng(image, file);
        std::printf("  %s %s (%dx%d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
        if (!ok) ++failures;
        check(image.getWidth() == ed.getWidth() && image.getHeight() == ed.getHeight(), "snapshot matches editor size");
    };

    const float scales[] = {0.6f, 1.0f, 2.0f};
    for (float s : scales) {
        ed.setScale(s);
        pump(60);
        for (int v = 0; v < mw909::ui::kNumViews; ++v) {
            ed.setView(static_cast<View>(v));
            pump(40);
            shoot(juce::String(kViewFileNames[v]) + "-" + juce::String(juce::roundToInt(s * 100)));
        }
    }

    // States at 1.0x.
    ed.setScale(1.0f);
    pump(60);
    auto* strip = findOne<mw909::ui::SequencerStrip>(ed);
    auto* header = findOne<mw909::ui::HeaderBar>(ed);

    // Step-edit modes on the SD track (accent / flam / sub / prob badges).
    ed.setView(View::Panel);
    for (int m = 0; m < 4; ++m) {
        if (strip != nullptr) mouse.click(*findButton(*strip, mw909::ui::kStepModeNames[m]));
        if (strip != nullptr) mouse.click(*findButton(*strip, "SD"));
        shoot(juce::String("states-mode-") + juce::String(mw909::ui::kStepModeNames[m]).toLowerCase());
    }
    if (strip != nullptr) mouse.click(*findButton(*strip, "LEVEL"));

    // The settings menu open over the PANEL view, with a submenu.
    if (header != nullptr) {
        mouse.click(*findButton(*header, "SETUP"));
        pump(60);
        if (auto* m = topModal()) {
            for (int i = 0; i < 5; ++i) m->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
            m->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        }
        pump(60);
        shoot("states-settings-menu");
        juce::PopupMenu::dismissAllActiveMenus();
        pump(60);
    }
    // A knob's right-click menu.
    if (auto* k = findControl(ed, "bd_decay")) {
        mouse.rightClick(*k);
        pump(60);
        shoot("states-knob-menu");
        juce::PopupMenu::dismissAllActiveMenus();
        pump(60);
    }
    // The mixer with the SD strip expanded, and the OUT menu.
    ed.setView(View::Mixer);
    pump(40);
    if (auto* mixer = findOne<mw909::ui::MixerView>(ed)) {
        mixer->setExpanded(mw909::SD);
        processor->apvts().getParameter("sd_eq_on")->setValueNotifyingHost(1.0f);
        pump(40);
        shoot("states-mixer-strip");
        if (auto* box = findBound<mw909::ui::ParamValueBox>(ed, "bd_out")) {
            mouse.click(*box);
            pump(60);
            shoot("states-mixer-out-menu");
            juce::PopupMenu::dismissAllActiveMenus();
            pump(60);
        }
        ed.setScale(2.0f);
        pump(60);
        shoot("states-mixer-strip-200");
        ed.setScale(0.6f);
        pump(60);
        shoot("states-mixer-strip-60");
        ed.setScale(1.0f);
        pump(60);
        mixer->setExpanded(-1);
    }
    // FX: reverb and the filter selected, a few effects on.
    ed.setView(View::Fx);
    pump(40);
    if (auto* fx = findOne<mw909::ui::FxView>(ed)) {
        setParam(*processor, "fx_reverb_on", 1.0f);
        setParam(*processor, "fx_delay_on", 1.0f);
        setParam(*processor, "mcomp_on", 1.0f);
        setParam(*processor, "trig1_on", 1.0f);
        for (int id : {4, 9, 1, 7}) {
            fx->rack().onSelect(id);
            pump(40);
            shoot("states-fx-" + mw909::ui::FxRack::shortName(id).toLowerCase().removeCharacters(" "));
        }
    }
    // The song editor (chain and track tabs) over the GRID.
    ed.setView(View::Grid);
    processor->patterns().editChain([](mw909::seq::Chain& c) {
        c.length = 6;
        for (int i = 0; i < 6; ++i) c.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 3);
    });
    processor->patterns().editSongTrack(0, [](mw909::seq::SongTrack& t) {
        for (int i = 0; i < 20; ++i) mw909::seq::insertMeasure(t, t.length, i % 5);
    });
    ed.showSongEditor(true);
    pump(40);
    shoot("states-chain-editor");
    if (auto* se = ed.songEditor()) se->setTab(true);
    pump(40);
    shoot("states-track-editor");
    ed.showSongEditor(false);
    pump(40);
    // Authentic editing: the question over a rich pattern, then on (pattern kept).
    ed.setView(View::Panel);
    if (strip != nullptr) {
        const auto before = processor->patterns().pattern(processor->patterns().selectedPattern());
        mouse.click(strip->authenticButton());
        pump(40);
        shoot("states-authentic-menu");
        chooseMenuItem(1);
        pump(40);
        shoot("states-authentic");
        check(processor->patterns().pattern(processor->patterns().selectedPattern()) == before, "AUTHENTIC on keeps the pattern");
        mouse.click(strip->authenticButton());
        pump(20);
    }
    processor->patterns().edit(processor->patterns().selectedPattern(), [](mw909::seq::Pattern& p) { p.length = 40; });
    if (strip != nullptr) mouse.click(*strip->pageButton(1));
    ed.setView(View::Grid);
    pump(40);
    shoot("states-grid-40-steps");
    // Stopped.
    setParam(*processor, paramid::seqRun, 0.0f);
    advanceTransport(*processor, 4);
    ed.setView(View::Panel);
    pump(40);
    ed.pollNow();
    shoot("states-stopped");
    return failures == 0 ? 0 : 1;
}

// --- more states (a second pass over what a reviewer would poke at) -------------------------

int states(const juce::File& dir) {
    using namespace mw909;
    dir.createDirectory();
    auto processor = makeProcessor();
    setParam(*processor, paramid::seqMode, 2.0f);
    setParam(*processor, paramid::seqRun, 1.0f);
    decoratePattern(*processor);
    processor->serviceMessageThread();
    advanceTransport(*processor, 40);
    processor->serviceMessageThread();
    OpenEditor open(*processor);
    if (open.editor == nullptr || open.peer == nullptr) return 1;
    auto& ed = *open.editor;
    Mouse mouse{open.peer};
    auto snapshot = [&] { return ed.createComponentSnapshot(ed.getLocalBounds(), true, 1.0f); };
    auto shoot = [&](const juce::String& name, bool advance = true) {
        mouse.move({3.0f, 3.0f});  // park the mouse on the wordmark: no hover, no tooltip
        ed.pollNow();
        if (advance) advanceTransport(*processor, 6);
        processor->serviceMessageThread();
        ed.pollNow();
        pump(40);
        const juce::Image image = snapshot();
        const juce::File file = dir.getChildFile(name + ".png");
        const bool ok = savePng(image, file);
        std::printf("  %s %s (%dx%d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
        if (!ok) ++failures;
    };
    auto* strip = findOne<ui::SequencerStrip>(ed);
    auto* header = findOne<ui::HeaderBar>(ed);
    if (strip == nullptr || header == nullptr) return 1;

    // The queued pattern blinks its whole key: compare the key across phases.
    ed.setView(ui::View::Panel);
    pump(40);
    {
        auto* key = strip->patternButton(4);
        mouse.click(*key);  // playing: queued for the next measure
        advanceTransport(*processor, 1);  // the audio thread publishes the queue
        processor->serviceMessageThread();
        check(processor->playState().queuedPattern.load() == seq::patternIndex(0, 0, 4), "pattern 5 queued while playing");
        double maxDiff = 0.0;
        juce::Colour first;
        for (int i = 0; i < 24; ++i) {
            ed.pollNow();
            pump(5);
            const auto img = key->createComponentSnapshot(key->getLocalBounds().reduced(4), true, 1.0f);
            const auto c = img.getPixelAt(img.getWidth() / 2, img.getHeight() - 3);
            if (i == 0) first = c;
            const double d = std::abs(c.getFloatRed() - first.getFloatRed()) + std::abs(c.getFloatGreen() - first.getFloatGreen()) +
                             std::abs(c.getFloatBlue() - first.getFloatBlue());
            maxDiff = juce::jmax(maxDiff, d);
            if (i == 4) shoot("states-queued-a", false);
            if (i == 12) shoot("states-queued-b", false);
        }
        std::printf("  queued key face: largest colour change over two blink phases %.2f (RGB sum, 0..3)\n", maxDiff);
        check(maxDiff > 0.3, "the queued pattern key visibly blinks (face changes, not an outline)");
        mouse.click(*strip->patternButton(0), juce::ModifierKeys::shiftModifier);  // back to 1, now
        pump(20);
    }

    // Long preset name, user ROM with a long name, mute + solo (implied mutes dimmed).
    processor->presets().saveUser(plugin::PresetKind::Full, "Warehouse Rave Kit With Extra Long Descriptive Name (final v3)");
    const auto wav = writeTestWav(scratchDir().getChildFile("My long user crash cymbal sample name final mix.wav"), 44100.0, 0.5);
    processor->importUserRom(wav, RomRegion::Crash);
    setParam(*processor, paramid::knob(CR, knob::PcmSource), 2.0f);
    setParam(*processor, "sd_mute", 1.0f);
    setParam(*processor, "rd_solo", 1.0f);
    pump(60);
    shoot("states-panel-solo-userrom");
    ed.setView(ui::View::Mixer);
    pump(40);
    shoot("states-mixer-solo");
    ed.setView(ui::View::Panel);
    pump(40);
    {
        juce::Array<juce::TextButton*> buttons; collect(*header, buttons);
        for (auto* b : buttons)
            if (b->getButtonText().startsWith("Warehouse")) {
                mouse.click(*b);
                pump(60);
                shoot("states-preset-list-menu");
                if (auto* m = topModal()) {
                    for (int k = 0; k < 3; ++k) m->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                    m->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
                }
                pump(60);
                shoot("states-preset-list-submenu");
                juce::PopupMenu::dismissAllActiveMenus();
                pump(60);
                break;
            }
    }
    setParam(*processor, "sd_mute", 0.0f);
    setParam(*processor, "rd_solo", 0.0f);

    // Menus at the scale extremes (they follow the UI scale).
    for (float sc : {2.0f, 0.6f}) {
        ed.setScale(sc);
        pump(60);
        mouse.click(*findButton(*header, "SETUP"));
        pump(60);
        shoot("states-settings-menu-" + juce::String(juce::roundToInt(sc * 100)));
        juce::PopupMenu::dismissAllActiveMenus();
        pump(60);
        if (auto* k = findControl(ed, "bd_decay")) {
            mouse.rightClick(*k);
            pump(60);
            shoot("states-knob-menu-" + juce::String(juce::roundToInt(sc * 100)));
            juce::PopupMenu::dismissAllActiveMenus();
            pump(60);
        }
    }
    ed.setScale(1.0f);
    pump(60);

    // Value readout text entry in progress.
    if (auto* acc = findControl(ed, paramid::totalAccent)) {
        juce::Array<juce::Label*> labels; collect(*acc, labels);
        if (!labels.isEmpty()) {
            labels[0]->showEditor();
            pump(40);
            shoot("states-text-entry");
            labels[0]->hideEditor(true);
        }
    }

    // A 64-step pattern on page 4.
    processor->patterns().edit(processor->patterns().selectedPattern(), [](seq::Pattern& p) {
        p.length = 64;
        for (int st = 48; st < 64; st += 3) p.at(0, BD, st).setLevel(seq::StepLevel::Normal);
    });
    pump(20);
    mouse.click(*strip->pageButton(3));
    shoot("states-panel-page4");
    ed.setView(ui::View::Grid);
    pump(40);
    shoot("states-grid-64");
    processor->patterns().edit(processor->patterns().selectedPattern(), [](seq::Pattern& p) { p.length = 16; });
    mouse.click(*strip->pageButton(0));

    // A full song track and chain in the editor.
    processor->patterns().editSongTrack(0, [](seq::SongTrack& t) {
        t = {};
        for (int i = 0; i < seq::kMaxSongLength; ++i) seq::insertMeasure(t, t.length, i % 16);
    });
    processor->patterns().editChain([](seq::Chain& c) {
        c.length = seq::kMaxChainLength;
        for (int i = 0; i < seq::kMaxChainLength; ++i) c.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i % 40);
    });
    ed.showSongEditor(true);
    pump(40);
    shoot("states-chain-64");
    if (auto* se = ed.songEditor()) {
        se->setTab(true);
        pump(40);
        shoot("states-track-256");
        se->scrollToRow(1000);
        pump(40);
        shoot("states-track-256-end");
    }
    ed.showSongEditor(false);
    pump(40);

    // Every effect selected, and the mixer with strips expanded at the edges.
    ed.setView(ui::View::Fx);
    pump(40);
    if (auto* fx = findOne<ui::FxView>(ed))
        for (int id = 0; id < kNumFxInstances; ++id) {
            fx->rack().onSelect(id);
            pump(30);
            shoot("states-fx-all-" + juce::String(id) + "-" + ui::FxRack::shortName(id).toLowerCase().removeCharacters(" "));
        }
    ed.setView(ui::View::Mixer);
    pump(40);
    if (auto* mixer = findOne<ui::MixerView>(ed))
        for (int v : {static_cast<int>(BD), static_cast<int>(RD)}) {
            mixer->setExpanded(v);
            pump(40);
            shoot("states-mixer-expanded-" + ui::voiceCode(v).toLowerCase());
        }
    // The authentic menus on the strip.
    ed.setView(ui::View::Panel);
    pump(40);
    mouse.rightClick(strip->authenticButton());
    pump(40);
    shoot("states-authentic-rightclick");
    juce::PopupMenu::dismissAllActiveMenus();
    pump(40);
    auto& pm = processor->presets();
    for (int i = pm.numPresets(plugin::PresetKind::Full) - 1; i >= 0; --i)
        if (!pm.preset(plugin::PresetKind::Full, i).factory) pm.deleteUser(plugin::PresetKind::Full, i);
    return failures == 0 ? 0 : 1;
}

// --- stress --------------------------------------------------------------------------------

int stress(int iterations) {
    auto processor = makeProcessor();
    setParam(*processor, paramid::seqMode, 1.0f);  // Host Sync, the host thread plays
    AudioThread audio(*processor, true);
    juce::Random rng(1234);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < iterations; ++i) {
        std::unique_ptr<juce::AudioProcessorEditor> base(processor->createEditor());
        auto* editor = dynamic_cast<mw909Editor*>(base.get());
        if (editor == nullptr) { check(false, "editor type"); break; }
        editor->addToDesktop(kWindowFlags);
        editor->setVisible(true);
        editor->setView(static_cast<View>(i % mw909::ui::kNumViews));
        pump(20);
        editor->setScale(0.6f + 1.4f * rng.nextFloat());
        pump(20);
        if (i % 7 == 0) processor->setCurrentProgram(rng.nextInt(juce::jmax(1, processor->getNumPrograms())));
        if (i % 11 == 0) {
            juce::MemoryBlock state;
            processor->getStateInformation(state);
            processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        if (i % 5 == 0) editor->showSongEditor(true);
        if (i % 4 == 2)
            if (auto* mixer = findOne<mw909::ui::MixerView>(*editor)) mixer->setExpanded(rng.nextInt(mw909::kNumVoices));
        if (i % 4 == 3)
            if (auto* fx = findOne<mw909::ui::FxView>(*editor)) fx->rack().onSelect(rng.nextInt(mw909::kNumFxInstances));
        processor->patterns().edit(processor->patterns().selectedPattern(), [&](mw909::seq::Pattern& p) {
            p.at(0, rng.nextInt(mw909::seq::kNumTracks), rng.nextInt(16)).setLevel(static_cast<mw909::seq::StepLevel>(rng.nextInt(4)));
        });
        processor->audition(static_cast<mw909::VoiceId>(rng.nextInt(mw909::kNumVoices)), rng.nextBool());
        pump(20);
        editor->removeFromDesktop();
        base.reset();
        // The editor's gone; its timers and listeners must be gone too.
        processor->setStateInformation(nullptr, 0);
        pump(5);
    }
    const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::printf("  %d open/close cycles in %.1f s, audio thread processed %ld blocks\n", iterations, elapsed, audio.blocks());
    check(audio.blocks() > 0, "audio thread kept running");
    return failures == 0 ? 0 : 1;
}

// --- idle CPU ------------------------------------------------------------------------------

int idle(int seconds) {
    auto processor = makeProcessor();
    {
        // Baseline: the message loop and the processor's own 30 Hz service timer, no editor.
        pump(200);
        const double c0 = cpuSeconds();
        pump(seconds * 1000);
        const double used = cpuSeconds() - c0;
        std::printf("  baseline (no editor): %.1f ms CPU over %d s = %.2f%% of one core\n", used * 1000.0, seconds,
                    used / seconds * 100.0);
    }
    OpenEditor open(*processor);
    if (open.editor == nullptr) return 1;
    pump(300);  // first paint, caches
    // The product budget is 5 % of a core. Under ASan / TSan the figure is
    // mostly instrumentation (TSan's baseline without an editor is already
    // ~3.6 %, an idle view ~6 %), so those builds check only for a runaway
    // editor: a repaint or timer loop costs far more than 20 % there.
    const double limit = kSanitizerBuild ? 0.20 : 0.05;
    const char* const what = kSanitizerBuild ? "idle editor below 20% of a core (sanitizer build: runaway guard)"
                                             : "idle editor below 5% of a core";
    for (int v = 0; v < mw909::ui::kNumViews; ++v) {
        open.editor->setView(static_cast<View>(v));
        settle(open.peer, 300);
        const double c0 = cpuSeconds();
        pump(seconds * 1000);
        const double used = cpuSeconds() - c0;
        std::printf("  idle %-5s (no audio): %.1f ms CPU over %d s = %.2f%% of one core\n", mw909::ui::kViewNames[v], used * 1000.0,
                    seconds, used / seconds * 100.0);
        check(used / seconds < limit, what);
    }
    {
        open.editor->setView(View::Panel);
        setParam(*processor, paramid::seqMode, 1.0f);
        AudioThread audio(*processor, true);
        pump(300);
        const double c0 = cpuSeconds();
        pump(seconds * 1000);
        const double used = cpuSeconds() - c0;
        std::printf("  playing (audio thread + UI): %.1f ms CPU over %d s = %.2f%% of one core (incl. DSP)\n", used * 1000.0, seconds,
                    used / seconds * 100.0);
    }
    return failures == 0 ? 0 : 1;
}

// --- interaction ------------------------------------------------------------------------------

int interact() {
    auto processor = makeProcessor();
    OpenEditor open(*processor);
    if (open.editor == nullptr) return 1;
    auto* editor = open.editor;
    editor->setView(View::Panel);
    pump(200);
    check(open.peer != nullptr, "editor has a desktop peer under Xvfb");
    if (open.peer == nullptr) return 1;
    Mouse mouse{open.peer};

    // Knobs: drag, shift-drag fine, wheel, double-click reset, text entry.
    auto* bdLevel = findControl(*editor, "bd_level");
    check(bdLevel != nullptr, "BD LEVEL knob found in PANEL view");
    if (bdLevel != nullptr) {
        const float before = raw(*processor, "bd_level");
        mouse.dragVertical(*bdLevel, -40);
        const float after = raw(*processor, "bd_level");
        check(after > before, "drag up raises BD level");
        mouse.dragVertical(*bdLevel, 10, juce::ModifierKeys::shiftModifier);
        const float fine = raw(*processor, "bd_level");
        check(fine < after && after - fine < 0.02f, "shift-drag is fine");
        mouse.wheel(*bdLevel, 1.0f);
        check(raw(*processor, "bd_level") > fine, "wheel up raises the value");
        mouse.doubleClick(*bdLevel);
        check(std::abs(raw(*processor, "bd_level") - 0.5f) < 1.0e-4f, "double-click resets to default (0.5)");
    }
    // An extended (flat-cap) knob and a bipolar semitone knob.
    if (auto* pitch = findControl(*editor, "bd_pitch")) {
        mouse.dragVertical(*pitch, -20);
        check(raw(*processor, "bd_pitch") > 0.5f, "extended BD PITCH knob drags up");
    } else check(false, "BD PITCH knob");

    // Right-click opens the MIDI learn menu, which then goes away.
    if (bdLevel != nullptr) {
        mouse.rightClick(*bdLevel);
        pump(50);
        check(juce::PopupMenu::dismissAllActiveMenus(), "right-click opened a popup menu");
        pump(30);
    }

    // Step keys: click cycles, shift-click weak, drag paints.
    juce::Array<mw909::ui::StepKey*> keys;
    collect(*editor, keys);
    check(keys.size() == 16, "16 step keys");
    auto& model = processor->patterns();
    const int pat = model.selectedPattern();
    model.edit(pat, [](mw909::seq::Pattern& p) { mw909::seq::clearPattern(p); p.length = 16; for (auto& l : p.trackLength) l = 0; });
    if (auto* b = findButton(*editor, "LEVEL")) mouse.click(*b);
    pump(20);
    if (keys.size() == 16) {
        auto key = [&](int i) -> mw909::ui::StepKey* { for (auto* k : keys) if (k->index() == i) return k; return nullptr; };
        if (auto* b = findButton(*editor, "BD")) mouse.click(*b);
        mouse.click(*key(0));
        check(model.pattern(pat).at(0, 0, 0).level() == mw909::seq::StepLevel::Normal, "click: off -> normal");
        mouse.click(*key(0));
        check(model.pattern(pat).at(0, 0, 0).level() == mw909::seq::StepLevel::Accent, "click: normal -> accent");
        mouse.click(*key(0));
        check(model.pattern(pat).at(0, 0, 0).level() == mw909::seq::StepLevel::Off, "click: accent -> off");
        mouse.click(*key(0), juce::ModifierKeys::shiftModifier);
        check(model.pattern(pat).at(0, 0, 0).level() == mw909::seq::StepLevel::Weak, "shift-click: weak");
        mouse.drag(mouse.at(*key(4)), mouse.at(*key(7)));
        bool painted = true;
        for (int s = 4; s <= 7; ++s) painted = painted && model.pattern(pat).at(0, 0, s).level() == mw909::seq::StepLevel::Normal;
        check(painted, "drag paints steps 5..8 normal");
    }

    // GRID view click.
    editor->setView(View::Grid);
    pump(50);
    if (auto* grid = findOne<mw909::ui::GridView>(*editor)) {
        mouse.clickAt(mouse.at(*grid, grid->cellCentre(mw909::SD, 2)));
        check(model.pattern(pat).at(0, mw909::SD, 2).level() == mw909::seq::StepLevel::Normal, "grid click sets SD step 3");
    } else check(false, "grid view present");

    // MIXER: fader drag.
    editor->setView(View::Mixer);
    pump(50);
    if (auto* sdFader = findControl(*editor, "sd_level")) {
        const float before = raw(*processor, "sd_level");
        mouse.dragVertical(*sdFader, 30);
        check(raw(*processor, "sd_level") < before, "fader drag down lowers SD level");
    } else check(false, "SD level fader found in MIXER view");

    // FX: a knob of the selected effect.
    editor->setView(View::Fx);
    pump(50);
    if (auto* fx = findOne<mw909::ui::FxView>(*editor)) {
        fx->rack().onSelect(4);  // reverb
        pump(20);
        if (auto* decay = findControl(*editor, "fx_reverb_decay")) {
            const float before = raw(*processor, "fx_reverb_decay");
            mouse.dragVertical(*decay, -30);
            check(raw(*processor, "fx_reverb_decay") > before, "FX reverb DECAY knob drags up");
        } else check(false, "reverb DECAY knob");
    }

    // Buttons: view switch via header, run.
    editor->setView(View::Panel);
    pump(30);
    auto* gridButton = findButton(*editor, "GRID");
    auto* run = findButton(*editor, "START / STOP");
    check(gridButton != nullptr && run != nullptr, "header and transport buttons found");
    if (gridButton != nullptr) {
        mouse.click(*gridButton);
        check(editor->view() == View::Grid, "clicking GRID switches the view");
    }
    if (run != nullptr) {
        mouse.click(*run);
        check(raw(*processor, paramid::seqRun) > 0.5f, "START/STOP sets seq_run");
        mouse.click(*run);
        check(raw(*processor, paramid::seqRun) < 0.5f, "START/STOP clears seq_run");
    }

    // Resize via setScale persists to the UI state.
    editor->setScale(1.5f);
    pump(30);
    check(std::abs(processor->uiState().scale - 1.5f) < 0.02f, "scale persisted in UI state");
    check(editor->getWidth() == 1920 && editor->getHeight() == 1140, "1.5x window is 1920x1140");
    // A new editor opens at the stored scale.
    {
        std::unique_ptr<juce::AudioProcessorEditor> second(processor->createEditor());
        std::printf("  second editor: %dx%d (stored scale %.2f)\n", second->getWidth(), second->getHeight(), static_cast<double>(processor->uiState().scale));
        check(second->getWidth() == 1920 && second->getHeight() == 1140, "a new editor opens at the stored scale");
    }
    return failures == 0 ? 0 : 1;
}

// --- wiring: every §7 control path reaches the processor -----------------------------------

// Counts host gestures so a test can prove none is left open.
struct GestureCounter final : juce::AudioProcessorListener {
    int begins = 0, ends = 0;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++begins; }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { ++ends; }
};

int wiring() {
    using namespace mw909;
    auto processor = makeProcessor();
    GestureCounter gestures;
    processor->addListener(&gestures);  // the tool, not the editor, listens
    OpenEditor open(*processor);
    if (open.editor == nullptr || open.peer == nullptr) { check(false, "editor + peer"); return 1; }
    auto& ed = *open.editor;
    Mouse mouse{open.peer};
    auto rawp = [&](const juce::String& id) { return raw(*processor, id); };
    auto& model = processor->patterns();
    auto* strip = findOne<ui::SequencerStrip>(ed);
    check(strip != nullptr, "sequencer strip present");
    if (strip == nullptr) return 1;
    auto click = [&](juce::Component* c, juce::ModifierKeys mods = {}) {
        if (c == nullptr) { check(false, "control to click exists"); return; }
        mouse.click(*c, mods);
        pump(10);
    };

    // --- bank / group / pattern selection (stopped: immediate) ---
    click(strip->bankButton(1));
    click(strip->groupButton(1));
    click(strip->patternButton(2));
    check(model.selectedPattern() == seq::patternIndex(1, 1, 2), "BANK II + GROUP 2 + pattern 3 selects II-2-03");
    click(strip->bankButton(0));
    click(strip->groupButton(0));
    click(strip->patternButton(0));
    check(model.selectedPattern() == 0, "BANK I + GROUP 1 + pattern 1 selects I-1-01");
    const int pat = model.selectedPattern();
    model.edit(pat, [](seq::Pattern& p) { seq::clearPattern(p); p.length = 16; for (auto& l : p.trackLength) l = 0; });

    // --- track selection + step modes ---
    juce::Array<ui::StepKey*> keys;
    collect(ed, keys);
    auto key = [&](int i) -> ui::StepKey* { for (auto* k : keys) if (k->index() == i) return k; return nullptr; };
    click(findButton(*strip, "SD"));
    check(processor->uiState().selectedTrack == SD, "track button SD selects the SD track");
    click(key(2));
    check(model.pattern(pat).at(0, SD, 2).level() == seq::StepLevel::Normal, "key click edits the selected track (SD)");
    click(findButton(*strip, "FLAM"));
    check(processor->uiState().stepEditMode == 1, "FLAM mode button");
    click(key(2));
    check(model.pattern(pat).at(0, SD, 2).flam(), "FLAM mode: click toggles flam on an active step");
    click(findButton(*strip, "SUB"));
    click(key(2));
    click(key(2));
    check(model.pattern(pat).at(0, SD, 2).subSteps() == 3, "SUB mode: two clicks give x3");
    click(key(2), juce::ModifierKeys::shiftModifier);
    check(model.pattern(pat).at(0, SD, 2).subSteps() == 2, "SUB mode: shift-click steps back");
    click(findButton(*strip, "PROB"));
    click(key(2));
    check(model.pattern(pat).at(0, SD, 2).probabilityCode() == 1, "PROB mode: click lowers the probability one notch");
    click(key(5));
    check(model.pattern(pat).at(0, SD, 5).level() == seq::StepLevel::Normal && !model.pattern(pat).at(0, SD, 5).flam(),
          "PROB mode on an empty step: switches it on, nothing else");
    click(findButton(*strip, "LEVEL"));
    click(findButton(*strip, "AC"));
    click(key(0));
    check(model.pattern(pat).at(0, seq::kAccentTrack, 0).on(), "AC track: click sets an accent step");
    click(findButton(*strip, "BD"));

    // --- right-click step menu: "Accent" (Off, Weak, Normal, Accent -> 4 downs) ---
    mouse.rightClick(*key(9));
    check(chooseMenuItem(4), "step menu opened");
    check(model.pattern(pat).at(0, BD, 9).level() == seq::StepLevel::Accent, "step menu: Accent");

    // --- pages follow LAST STEP ---
    auto* page2 = strip->pageButton(1);
    check(!page2->isEnabled(), "page 2 disabled at last step 16");
    model.edit(pat, [](seq::Pattern& p) { p.length = 32; });
    pump(10);
    check(page2->isEnabled(), "page 2 enabled once LAST STEP is 32 (no other event needed)");
    click(page2);
    check(processor->uiState().page == 1, "page 2 button selects page 2");
    click(key(0));
    check(model.pattern(pat).at(0, BD, 16).on(), "key 1 on page 2 edits step 17");
    click(strip->pageButton(0));
    check(processor->uiState().page == 0, "page 1 button selects page 1");

    // --- pattern fields ---
    {
        const int before = model.pattern(pat).length;
        mouse.wheel(*strip->lastStepBox(), 0.5f);
        check(model.pattern(pat).length > before, "LAST STEP box: wheel raises the length");
        model.edit(pat, [](seq::Pattern& p) { p.length = 16; });
        click(strip->scaleBox());
        check(chooseMenuItem(3), "SCALE menu opened");
        check(model.pattern(pat).scale == seq::Scale::EighthTriplet && model.pattern(pat).length == 12,
              "SCALE menu: 1/8T, LAST STEP follows to 12");
        click(strip->scaleBox());
        chooseMenuItem(1);
        check(model.pattern(pat).scale == seq::Scale::Sixteenth && model.pattern(pat).length == 16, "SCALE back to 1/16, LAST STEP 16");
        const int flamBefore = model.pattern(pat).flamMs;
        mouse.wheel(*strip->flamBox(), 0.5f);
        check(model.pattern(pat).flamMs > flamBefore, "FLAM box: wheel lengthens the flam interval");
        click(strip->shuffleKey(3));
        check(model.pattern(pat).shuffle == seq::shuffleTicksForKey(4), "SHUFFLE key 4 sets 6/96");
        check(strip->shuffleKey(3)->getToggleState(), "SHUFFLE key 4 lit");
        mouse.wheel(*strip->shuffleFineBox(), 0.5f);
        check(model.pattern(pat).shuffle > seq::shuffleTicksForKey(4), "SHUFFLE FINE: wheel adds ticks");
        check(!strip->shuffleKey(3)->getToggleState(), "SHUFFLE key goes dark off the hardware grid");
        click(findButton(*strip, "CH"));
        mouse.wheel(*strip->trackLengthBox(), 0.5f);
        mouse.wheel(*strip->trackLengthBox(), 0.5f);
        check(model.pattern(pat).trackLength[CH] > 0, "TRACK LEN: wheel gives CH its own length");
        click(findButton(*strip, "BD"));
    }
    {
        auto* autoFill = findBound<ui::ParamValueBox>(*strip, paramid::autoFill);
        if (autoFill != nullptr) {
            click(autoFill);
            check(chooseMenuItem(5), "AUTO FILL menu opened");
            check(juce::roundToInt(rawp(paramid::autoFill)) == 4, "AUTO FILL: every 4");
        } else check(false, "AUTO FILL box");
    }

    // --- variation / tap write / play mode / run ---
    {
        juce::Array<ui::ChoiceButtons*> choices; collect(*strip, choices);
        ui::ChoiceButtons* variation = nullptr;
        ui::ChoiceButtons* playMode = nullptr;
        for (auto* c : choices) {
            if (c->parameter().getParameterID() == paramid::variation) variation = c;
            if (c->parameter().getParameterID() == paramid::playMode) playMode = c;
        }
        if (variation != nullptr) {
            click(variation->button(1));
            check(juce::roundToInt(rawp(paramid::variation)) == 1, "variation AB");
            click(variation->button(2));
            click(key(3));
            check(model.pattern(model.selectedPattern()).at(1, BD, 3).on(), "variation B: key edits variation B");
            click(variation->button(0));
        } else check(false, "variation buttons");
        if (playMode != nullptr) {
            click(playMode->button(1));
            check(juce::roundToInt(rawp(paramid::playMode)) == 1, "PLAY MODE: CHAIN");
            click(playMode->button(2));
            check(juce::roundToInt(rawp(paramid::playMode)) == 2, "PLAY MODE: TRACK");
            click(playMode->button(0));
        } else check(false, "play mode buttons");
    }
    click(findButton(*strip, "TAP WRITE"));
    check(rawp(paramid::record) > 0.5f, "TAP WRITE sets record");
    click(findButton(*strip, "TAP WRITE"));
    check(rawp(paramid::record) < 0.5f, "TAP WRITE clears record");

    // --- song editor: chain and track ---
    click(findButton(*strip, "EDIT CHAIN / TRACK"));
    pump(30);
    if (auto* se = ed.songEditor()) {
        se->setTab(false);
        model.setChain({});
        click(findButton(*se, "ADD SELECTED"));
        click(findButton(*se, "ADD SELECTED"));
        check(model.chain().length == 2 && model.chain().entries[0] == pat, "CHAIN: ADD SELECTED appends the selected pattern");
        click(findButton(*se, "REMOVE LAST"));
        check(model.chain().length == 1, "CHAIN: REMOVE LAST");
        click(findButton(*se, "CLEAR"));
        check(model.chain().length == 0, "CHAIN: CLEAR empties the chain");
        click(findButton(*se, "TRACK"));
        check(se->editingTrack(), "TRACK tab");
        if (auto* ii2 = findButton(*se, "II-2")) {
            click(ii2);
            check(juce::roundToInt(rawp(paramid::songTrack)) == 5, "track selector II-2 sets song_track");
        } else check(false, "II-2 button");
        model.editSongTrack(5, [](seq::SongTrack& t) { t = {}; });
        click(findButton(*se, "ADD SELECTED"));
        click(findButton(*se, "ADD SELECTED"));
        click(findButton(*se, "ADD SELECTED"));
        check(model.songTrack(5).length == 3, "TRACK: ADD SELECTED appends measures");
        // Click the second chip: removes that measure.
        mouse.clickAt(mouse.at(*se, juce::Point<int>(12 + 27, se->getHeight() - 8 - 24 - 8 - 1 - 200)));
        click(findButton(*se, "REMOVE LAST"));
        check(model.songTrack(5).length == 2, "TRACK: REMOVE LAST");
        // A full 256-measure track scrolls: every measure can be reached and removed.
        model.editSongTrack(5, [](seq::SongTrack& t) {
            t = {};
            for (int m = 0; m < seq::kMaxSongLength; ++m) seq::insertMeasure(t, t.length, m % 16);
        });
        pump(10);
        check(se->entryVisible(0) && !se->entryVisible(seq::kMaxSongLength - 1), "TRACK 256: the last measure starts off screen");
        mouse.wheel(*se, -1.0f);
        check(se->firstRow() == 1, "TRACK 256: the wheel scrolls one row");
        se->scrollToRow(1000);
        check(se->entryVisible(seq::kMaxSongLength - 1) && !se->entryVisible(0), "TRACK 256: scrolled to the end");
        mouse.clickAt(mouse.at(*se, se->chipBounds(seq::kMaxSongLength - 1).getCentre()));
        check(model.songTrack(5).length == seq::kMaxSongLength - 1, "TRACK 256: clicking measure 256 removes it");
        click(findButton(*se, "CLEAR"));
        check(model.songTrack(5).length == 0 && se->firstRow() == 0, "TRACK: CLEAR empties it and scrolls back");
        click(findButton(*se, "CLOSE"));
        pump(50);
        check(ed.songEditor() == nullptr, "CLOSE removes the overlay");
        check(!findButton(*strip, "EDIT CHAIN / TRACK")->getToggleState(), "EDIT button follows CLOSE");
    } else check(false, "EDIT opens the song editor");

    // --- tools ---
    model.edit(pat, [](seq::Pattern& p) { seq::clearPattern(p); p.length = 16; for (auto& l : p.trackLength) l = 0; p.at(0, BD, 0).setLevel(seq::StepLevel::Normal); });
    click(findButton(*strip, "SHIFT >"));
    check(model.pattern(pat).at(0, BD, 1).on() && !model.pattern(pat).at(0, BD, 0).on(), "SHIFT > rotates later");
    click(findButton(*strip, "< SHIFT"));
    check(model.pattern(pat).at(0, BD, 0).on(), "< SHIFT rotates earlier");
    click(findButton(*strip, "COPY"));
    click(strip->patternButton(1));
    check(model.selectedPattern() == 1, "pattern 2 selected");
    click(findButton(*strip, "PASTE"));
    check(model.pattern(1).at(0, BD, 0).on(), "COPY / PASTE copies the pattern to I-1-02");
    click(findButton(*strip, "CLEAR"));
    check(!model.pattern(1).at(0, BD, 0).on(), "CLEAR clears the variation");
    click(findButton(*strip, "RANDOM"));
    {
        int on = 0;
        for (int t = 0; t < seq::kNumTracks; ++t) for (int st = 0; st < 16; ++st) on += model.pattern(1).at(0, t, st).on() ? 1 : 0;
        check(on > 0, "RANDOM writes steps");
        const auto seedA = strip->seed();
        const seq::Pattern a = model.pattern(1);
        click(findButton(*strip, ">"));
        check(strip->seed() == (seedA >= 9999 ? 1u : seedA + 1), "seed > steps the seed");
        check(!(model.pattern(1) == a), "seed > regenerates the pattern");
        click(findButton(*strip, "<"));
        check(strip->seed() == seedA && model.pattern(1) == a, "seed < returns to the same pattern (deterministic)");
    }
    click(findButton(*strip, "EUCLID"));
    check(chooseMenuItem(5), "EUCLID menu opened");
    {
        int on = 0;
        for (int st = 0; st < 16; ++st) on += model.pattern(1).at(0, BD, st).on() ? 1 : 0;
        check(on == 5, "EUCLID: 5 hits on BD");
    }
    // AUTHENTIC never rewrites a pattern by itself: turning it on over a
    // pattern the original could not hold asks (keep / conform); kept data
    // survives, new edits follow the rules; conforming can be undone.
    // RS step 7 starts empty whatever RANDOM left there (its seed comes from the
    // clock), so the two clicks on it below go off > normal > off.
    model.edit(1, [](seq::Pattern& p) {
        p.length = 40;
        p.at(0, RS, 6) = seq::Step{};
        p.at(0, RS, 3).setLevel(seq::StepLevel::Accent);
        p.at(0, BD, 2).setSubSteps(3);
        p.at(0, SD, 7).setLevel(seq::StepLevel::Weak);
    });
    const seq::Pattern rich = model.pattern(1);
    click(findButton(*strip, "AUTHENTIC"));
    check(topModal() != nullptr && !processor->uiState().authentic, "AUTHENTIC over a rich pattern asks first (menu, still off)");
    check(chooseMenuItem(1), "AUTHENTIC menu: on for new edits (keep the pattern)");
    check(processor->uiState().authentic, "AUTHENTIC sets the UI state");
    check(model.pattern(1) == rich, "AUTHENTIC on leaves the existing pattern untouched");
    check(strip->hintText().startsWith("AUTHENTIC: this pattern holds more"), "the hint says the pattern is outside the rules");
    check(!findButton(*strip, "SUB")->isEnabled(), "AUTHENTIC disables SUB");
    click(findButton(*strip, "RS"));
    click(key(6));
    click(key(6));
    check(!model.pattern(1).at(0, RS, 6).on(), "AUTHENTIC: RS cycles off > normal > off (no accent)");
    check(model.pattern(1).at(0, BD, 2).subSteps() == 3 && model.pattern(1).at(0, SD, 7).level() == seq::StepLevel::Weak &&
              model.pattern(1).length == 40 && model.pattern(1).at(0, RS, 3).level() == seq::StepLevel::Accent,
          "AUTHENTIC edits constrain only what they change (sub-step, weak, length, RS accent kept)");
    mouse.rightClick(strip->authenticButton());
    check(chooseMenuItem(1), "AUTHENTIC right-click menu: conform this pattern");
    check(seq::followsHardwareRules(model.pattern(1)) && processor->uiState().authentic, "conform: the pattern now follows the rules");
    const seq::Pattern conformed = model.pattern(1);
    click(findButton(*strip, "AUTHENTIC"));
    check(topModal() != nullptr, "AUTHENTIC off after conforming offers the undo");
    check(chooseMenuItem(2), "AUTHENTIC menu: turn off and restore");
    check(!processor->uiState().authentic, "AUTHENTIC off");
    // The two RS clicks (on, then off again) left the pattern as it was.
    check(model.pattern(1) == rich && !(conformed == rich), "restore brings back the pattern as it was before conforming");
    click(findButton(*strip, "AUTHENTIC"));
    check(chooseMenuItem(1) && processor->uiState().authentic, "AUTHENTIC on again (keep)");
    click(findButton(*strip, "AUTHENTIC"));
    check(topModal() == nullptr && !processor->uiState().authentic, "AUTHENTIC off with nothing to undo: no menu");
    click(findButton(*strip, "BD"));

    // --- MIDI drop in + export ---
    {
        const juce::File f = processor->exportPatternToMidiFile(1, 0);
        check(f.existsAsFile(), "exportPatternToMidiFile writes a .mid");
        auto* handle = findOne<ui::MidiDragHandle>(ed);
        model.edit(2, [](seq::Pattern& p) { seq::clearPattern(p); });
        click(strip->patternButton(2));
        if (handle != nullptr && f.existsAsFile()) {
            check(handle->isInterestedInFileDrag({f.getFullPathName()}), "drag handle accepts .mid");
            handle->filesDropped({f.getFullPathName()}, 5, 5);
            int on = 0;
            for (int st = 0; st < 16; ++st) on += model.pattern(2).at(0, BD, st).on() ? 1 : 0;
            check(on == 5, "dropping the .mid imports it into the selected pattern");
        }
    }

    // --- FILL reaches the sequencer: the fill pattern plays within two measures ---
    {
        click(strip->patternButton(0));
        setParam(*processor, paramid::seqMode, 2.0f);
        click(findButton(*strip, "START / STOP"));
        advanceTransport(*processor, 10);
        click(findButton(*strip, "FILL"));
        bool sawFill = false;
        const int start = processor->playState().currentPattern.load();
        for (int i = 0; i < 1500 && !sawFill; ++i) {
            advanceTransport(*processor, 1);
            sawFill = processor->playState().currentPattern.load() != start;
        }
        check(sawFill, "FILL: another (the fill) pattern plays");
        click(findButton(*strip, "START / STOP"));
        advanceTransport(*processor, 2);
    }

    // --- knob text entry ---
    if (auto* acc = findControl(ed, paramid::totalAccent)) {
        juce::Array<juce::Label*> labels; collect(*acc, labels);
        if (!labels.isEmpty()) {
            auto type = [&](const char* text) {
                labels[0]->showEditor();
                if (auto* te = labels[0]->getCurrentTextEditor()) te->setText(text, false);
                labels[0]->hideEditor(false);
                pump(5);
            };
            type("0.25");
            check(std::abs(rawp(paramid::totalAccent) - 0.25f) < 0.01f, "value readout text entry sets TOTAL ACCENT to 0.25");
            type("banana");
            check(std::abs(rawp(paramid::totalAccent) - 0.25f) < 0.01f, "text entry 'banana' changes nothing");
            check(labels[0]->getText().startsWith("0.25"), "text entry 'banana': the readout shows the value again");
            type("5");
            check(std::abs(rawp(paramid::totalAccent) - 1.0f) < 0.01f, "text entry '5' clamps to the maximum");
            type("");
            check(std::abs(rawp(paramid::totalAccent) - 1.0f) < 0.01f, "empty text entry changes nothing");
        } else check(false, "accent readout label");
    }

    // --- MIDI learn via the right-click menu ---
    if (auto* knob = findControl(ed, "bd_decay")) {
        processor->midiLearn().forget("bd_decay");
        mouse.rightClick(*knob);
        check(chooseMenuItem(2), "parameter menu opened");  // Reset, MIDI learn
        check(processor->midiLearn().isLearning() && processor->midiLearn().learningParameter() == "bd_decay",
              "menu 'MIDI learn' arms learn for bd_decay");
        juce::AudioBuffer<float> buffer(juce::jmax(2, processor->getTotalNumOutputChannels()), 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 102, 100), 0);
        processor->processBlock(buffer, midi);
        processor->serviceMessageThread();
        pump(20);
        check(processor->midiLearn().controllerFor("bd_decay") == 102, "CC 102 bound to bd_decay");
        mouse.rightClick(*knob);
        check(chooseMenuItem(3), "parameter menu reopened");  // Reset, MIDI learn, Forget
        check(processor->midiLearn().controllerFor("bd_decay") < 0, "menu 'Forget CC' unbinds");
    }

    // --- PANEL: audition + select via the name plate, mute, source, choke, user ROM ---
    {
        juce::Array<ui::NamePlate*> plates; collect(ed, plates);
        ui::NamePlate* cp = nullptr;
        for (auto* pl : plates) if (pl->isShowing() && pl->voice() == CP) cp = pl;
        if (cp != nullptr) {
            const auto before = processor->playState().hits[CP].load();
            click(cp);
            advanceTransport(*processor, 2);
            check(processor->uiState().selectedTrack == CP, "name plate selects its track");
            check(processor->playState().hits[CP].load() != before, "name plate auditions the voice");
        } else check(false, "CP name plate");
        if (auto* m = findBound<ui::ParamButton>(ed, "sd_mute")) {
            click(m);
            check(rawp("sd_mute") > 0.5f, "PANEL mute button sets sd_mute");
            click(m);
        } else check(false, "SD mute");
        if (auto* s = findBound<ui::ParamButton>(ed, "rd_solo")) {
            click(s);
            check(rawp("rd_solo") > 0.5f, "PANEL solo button sets rd_solo");
            click(s);
        } else check(false, "RD solo");
        juce::Array<ui::ChoiceButtons*> choices; collect(ed, choices);
        for (auto* c : choices)
            if (c->isShowing() && c->parameter().getParameterID() == "ch_source") {
                click(c->button(1));
                check(juce::roundToInt(rawp("ch_source")) == 1, "CH source SYN");
                click(c->button(0));
            }
        if (auto* mode = [&]() -> ui::ChoiceButtons* { for (auto* c : choices) if (c->isShowing() && c->parameter().getParameterID() == paramid::seqMode) return c; return nullptr; }()) {
            click(mode->button(3));
            check(juce::roundToInt(rawp(paramid::seqMode)) == 3, "sequencer MODE: HOST NOTE");
            click(mode->button(2));
        } else check(false, "sequencer mode buttons");
        if (auto* choke = findButton(ed, "CHOKE")) {
            const float before = rawp(paramid::hatChoke);
            click(choke);
            check(std::abs(rawp(paramid::hatChoke) - before) > 0.5f, "HI HAT CHOKE toggles hat_choke");
            click(choke);
        } else check(false, "CHOKE button");
        // User ROM: import through the processor (the file chooser is not driven
        // headless), the LOAD lamp follows, the LOAD menu clears it.
        const auto wav = writeTestWav(scratchDir().getChildFile("crash.wav"), 44100.0, 0.5);
        juce::Array<juce::TextButton*> buttons; collect(ed, buttons);
        juce::TextButton* crLoad = nullptr;
        int loads = 0;
        for (auto* b : buttons)
            if (b->getButtonText() == "LOAD" && b->isShowing() && loads++ == 2) crLoad = b;  // CH, OH, CR, RD
        check(processor->importUserRom(wav, RomRegion::Crash), "importUserRom (crash) succeeds");
        pump(40);
        check(crLoad != nullptr && crLoad->getToggleState(), "CR LOAD lamp lights with a user ROM");
        if (crLoad != nullptr) {
            mouse.rightClick(*crLoad);
            check(chooseMenuItem(2), "LOAD menu opened");
            pump(20);
            check(processor->userRom(RomSlot::Crash).empty(), "LOAD menu: Clear user ROM");
            check(!crLoad->getToggleState(), "CR LOAD lamp goes dark");
        }
    }

    // --- MIXER ---
    ed.setView(ui::View::Mixer);
    pump(40);
    if (auto* mixer = findOne<ui::MixerView>(ed)) {
        if (auto* pan = findControl(ed, "lt_pan")) {
            mouse.dragVertical(*pan, -30);
            check(rawp("lt_pan") > 0.05f, "MIXER pan knob");
        } else check(false, "lt_pan knob");
        for (const char* id : {"sd_spread", "sd_ch_drive", "sd_send"}) {
            if (auto* k = findControl(ed, id)) {
                mouse.dragVertical(*k, -30);
                check(rawp(id) > 0.05f, (juce::String("MIXER knob ") + id).toRawUTF8());
            } else check(false, id);
        }
        if (auto* out = findBound<ui::ParamValueBox>(*mixer, "sd_out")) {
            click(out);  // Main, Out 2, Out 3 -> 3 downs
            check(chooseMenuItem(3), "OUT menu opened");
            check(juce::roundToInt(rawp("sd_out")) == 2, "OUT menu assigns SD to Out 3");
        } else check(false, "OUT box");
        if (auto* route = findBound<ui::ParamValueBox>(*mixer, "sd_route")) {
            click(route);
            check(chooseMenuItem(2), "ROUTE menu opened");
            check(juce::roundToInt(rawp("sd_route")) == 1, "ROUTE: FX 1");
        } else check(false, "ROUTE box");
        if (auto* send = findBound<ui::ParamValueBox>(*mixer, "sd_send_bus")) {
            click(send);
            check(chooseMenuItem(3), "SEND menu opened");
            check(juce::roundToInt(rawp("sd_send_bus")) == 2, "SEND bus: FX 2");
        } else check(false, "SEND box");
        if (auto* pre = findBound<ui::ParamButton>(*mixer, "sd_send_pre")) {
            click(pre);
            check(rawp("sd_send_pre") > 0.5f, "SEND PRE toggles");
        } else check(false, "PRE button");
        juce::Array<ui::StripSummary*> summaries; collect(*mixer, summaries);
        if (summaries.size() == kNumVoices) {
            click(summaries[SD]);
            pump(20);
            check(mixer->expanded() == SD, "STRIP summary expands the SD channel");
            if (auto* eq = findBound<ui::ParamButton>(*mixer, "sd_eq_on")) {
                click(eq);
                check(rawp("sd_eq_on") > 0.5f, "strip EQ ON");
            } else check(false, "strip EQ ON");
            for (const char* id : {"sd_lowcut", "sd_eq1_gain", "sd_eq2_freq", "sd_comp_thresh", "sd_comp_makeup"}) {
                if (auto* k = findControl(*mixer, id)) {
                    const float before = rawp(id);
                    mouse.dragVertical(*k, -25);
                    if (std::abs(rawp(id) - before) <= 1.0e-3f) mouse.dragVertical(*k, 25);  // at its top already
                    check(std::abs(rawp(id) - before) > 1.0e-3f, (juce::String("strip knob ") + id).toRawUTF8());
                } else check(false, id);
            }
            for (const char* id : {"sd_comp_on", "sd_comp_pre", "sd_eq1_on", "sd_eq2_on"}) {
                if (auto* b = findBound<ui::ParamButton>(*mixer, id)) {
                    click(b);
                    check(rawp(id) > 0.5f, (juce::String("strip toggle ") + id).toRawUTF8());
                } else check(false, id);
            }
            click(summaries[SD]);
            pump(20);
            check(mixer->expanded() == -1, "STRIP summary collapses again");
        } else check(false, "11 strip summaries");
        check(findControl(ed, "fx1_level") != nullptr && findControl(ed, "master_volume") != nullptr, "mixer master column: FX return, volume");
    }

    // --- FX ---
    ed.setView(ui::View::Fx);
    pump(40);
    if (auto* fxView = findOne<ui::FxView>(ed)) {
        auto& rack = fxView->rack();
        // Defaults: dist, eqa, delay, dyna, filter on FX 1; the rest on FX 2.
        auto bus1 = rack.busOrder(0);
        check(bus1.size() == 5 && bus1[0] == 0, "FX 1 holds five effects, DIST first");
        // Lamp: switch DIST on.
        mouse.clickAt(mouse.at(rack, rack.lampBounds(0, 0).getCentre()));
        check(rawp("fx_dist_on") > 0.5f, "slot lamp switches DIST on");
        // Select REVERB (FX 2, second slot by default).
        const int reverbPos = rack.busOrder(1).indexOf(4);
        mouse.clickAt(mouse.at(rack, rack.slotBounds(1, reverbPos).getCentre().translated(0, 4)));
        check(fxView->effectEditor().instance() == 4, "clicking a slot selects REVERB for editing");
        check(findControl(ed, "fx_reverb_predelay") != nullptr && findControl(ed, "fx_reverb_mix") != nullptr,
              "REVERB controls shown");
        // Drag DIST from FX 1 to the end of FX 2.
        const auto from = mouse.at(rack, rack.slotBounds(0, 0).getCentre().translated(0, 6));
        const auto to = mouse.at(rack, rack.slotBounds(1, 6).getCentre());
        mouse.drag(from, to, {}, 10);
        check(juce::roundToInt(rawp("fx_dist_bus")) == 1, "drag moves DIST to FX 2");
        check(rack.busOrder(1).getLast() == 0, "DIST lands last on FX 2");
        check(rack.busOrder(0).size() == 4 && rack.busOrder(0)[0] == 1, "FX 1 renumbered (EQ A first)");
        // Reorder within FX 1: move the last slot to the front.
        const auto order = rack.busOrder(0);
        const int last = order.getLast();
        mouse.drag(mouse.at(rack, rack.slotBounds(0, order.size() - 1).getCentre().translated(0, 6)),
                   mouse.at(rack, rack.slotBounds(0, 0).getCentre().translated(-20, 0)), {}, 10);
        check(rack.busOrder(0)[0] == last, "drag reorders within FX 1");
        bool contiguous = true;
        for (int b = 0; b < 2; ++b) {
            const auto o = rack.busOrder(b);
            for (int i = 0; i < o.size(); ++i) contiguous = contiguous && juce::roundToInt(rawp(paramid::fx(o[i], "order"))) == i;
        }
        check(contiguous, "order keys stay contiguous on both buses");
        // Selected-effect controls of every kind: filter (choice, toggle, knob).
        fxView->rack().onSelect(9);
        pump(20);
        if (auto* type = findBound<ui::ParamValueBox>(ed, "fx_filter_type")) {
            click(type);
            check(chooseMenuItem(4), "filter TYPE menu");
            check(juce::roundToInt(rawp("fx_filter_type")) == 3, "filter TYPE: high-pass");
        } else check(false, "filter type box");
        if (auto* sync = findBound<ui::ParamButton>(ed, "fx_filter_lfo_sync")) {
            click(sync);
            check(rawp("fx_filter_lfo_sync") > 0.5f, "filter LFO SYNC toggle");
        } else check(false, "filter sync");
        if (auto* on = findBound<ui::ParamButton>(fxView->effectEditor(), "fx_filter_on")) {
            click(on);
            check(rawp("fx_filter_on") > 0.5f, "effect band ON button");
        } else check(false, "effect ON");
        // Returns.
        if (auto* k = findControl(ed, paramid::fx1To2)) {
            mouse.dragVertical(*k, -30);
            check(rawp(paramid::fx1To2) > 0.05f, "FX 1 -> FX 2 send knob");
        } else check(false, "fx1_to2");
        if (auto* box = findBound<ui::ParamValueBox>(ed, "fx2_out")) {
            click(box);
            check(chooseMenuItem(2), "FX 2 OUT menu");
            check(juce::roundToInt(rawp("fx2_out")) == 1, "FX 2 OUT: Out 2");
        } else check(false, "fx2_out");
        // Master chain.
        juce::Array<ui::ChoiceButtons*> choices; collect(*fxView, choices);
        for (auto* c : choices)
            if (c->parameter().getParameterID() == paramid::outputStage) {
                click(c->button(2));
                check(juce::roundToInt(rawp(paramid::outputStage)) == 2, "OUTPUT STAGE: ORIGINAL");
            }
        for (const char* id : {paramid::limiter, paramid::softClip, paramid::mcompOn}) {
            if (auto* b = findBound<ui::ParamButton>(ed, id)) {
                const float before = rawp(id);
                click(b);
                check(std::abs(rawp(id) - before) > 0.5f, (juce::String("master toggle ") + id).toRawUTF8());
            } else check(false, id);
        }
        for (const char* id : {"mcomp_mid_solo", "mcomp_high_on", "trig2_on"}) {
            if (auto* b = findBound<ui::ParamButton>(ed, id)) {
                const float before = rawp(id);
                click(b);
                check(std::abs(rawp(id) - before) > 0.5f, (juce::String("toggle ") + id).toRawUTF8());
            } else check(false, id);
        }
        for (const char* id : {"master_drive", "lim_thresh", "mcomp_xlow", "mcomp_low_ratio", "mcomp_high_makeup", "trig3_level", "trig1_pan"}) {
            if (auto* k = findControl(ed, id)) {
                const float before = rawp(id);
                mouse.dragVertical(*k, -25);
                if (std::abs(rawp(id) - before) <= 1.0e-3f) mouse.dragVertical(*k, 25);  // at its top already
                check(std::abs(rawp(id) - before) > 1.0e-3f, (juce::String("knob ") + id).toRawUTF8());
            } else check(false, id);
        }
        if (auto* src = findBound<ui::ParamValueBox>(ed, "trig1_source")) {
            click(src);
            check(chooseMenuItem(1), "trigger SOURCE menu");
            check(juce::roundToInt(rawp("trig1_source")) == 0, "trigger 1 SOURCE: BD");
        } else check(false, "trig1 source");
    }

    // --- GRID: lane header selects + auditions, drag paints along a lane, polymeter playheads ---
    ed.setView(ui::View::Grid);
    pump(40);
    if (auto* grid = findOne<ui::GridView>(ed)) {
        model.edit(model.selectedPattern(), [](seq::Pattern& p) { seq::clearPattern(p); p.length = 16; for (auto& l : p.trackLength) l = 0; });
        mouse.clickAt(mouse.at(*grid, grid->laneHeaderCentre(HT)));
        check(processor->uiState().selectedTrack == HT, "GRID lane header selects HT");
        mouse.drag(mouse.at(*grid, grid->cellCentre(CH, 0)), mouse.at(*grid, grid->cellCentre(CH, 5)), {}, 10);
        bool all = true;
        for (int st = 0; st <= 5; ++st) all = all && model.pattern(model.selectedPattern()).at(0, CH, st).on();
        check(all, "GRID drag paints CH steps 1..6");
        // Polymeter: CH loops over 3 steps, the pattern over 16.
        model.edit(model.selectedPattern(), [](seq::Pattern& p) { p.trackLength[CH] = 3; });
        setParam(*processor, paramid::seqMode, 2.0f);
        setParam(*processor, paramid::seqRun, 1.0f);
        bool exact = true;
        int checked = 0;
        for (int i = 0; i < 200; ++i) {
            advanceTransport(*processor, 3);
            ed.pollNow();
            const auto pos = processor->playState().position.load();
            const int step = static_cast<int>(pos & 0xff) - 1;
            if (step < 0 || !processor->playState().playing.load()) continue;
            const auto measure = static_cast<std::int64_t>(pos >> 8);
            ++checked;
            exact = exact && grid->playheadOf(CH) == static_cast<int>((measure * 16 + step) % 3) && grid->playheadOf(BD) == step;
        }
        check(checked > 50 && exact, "GRID playheads: BD follows the pattern, CH its own 3-step loop");
        setParam(*processor, paramid::seqRun, 0.0f);
        advanceTransport(*processor, 2);
    }

    // --- header: view switch, preset next / kind, settings menu ---
    auto* header = findOne<ui::HeaderBar>(ed);
    click(findButton(*header, "PANEL"));
    check(ed.view() == ui::View::Panel, "PANEL button");
    click(findButton(*header, "FX"));
    check(ed.view() == ui::View::Fx, "FX button");
    click(findButton(*header, "MIXER"));
    check(ed.view() == ui::View::Mixer, "MIXER button");
    {
        const auto before = processor->presets().currentName(plugin::PresetKind::Full);
        click(findButton(*header, ">"));
        check(processor->presets().currentName(plugin::PresetKind::Full) != before, "> loads the next full preset");
        click(findButton(*header, "<"));
        check(processor->presets().currentName(plugin::PresetKind::Full) == before, "< goes back");
        click(findButton(*header, "KIT"));
        const auto kitBefore = processor->presets().currentName(plugin::PresetKind::Kit);
        click(findButton(*header, ">"));
        check(processor->presets().currentName(plugin::PresetKind::Kit) != kitBefore, "KIT kind: > loads the next kit");
        click(findButton(*header, "FULL"));
    }
    {
        auto* setup = findButton(*header, "SETUP");
        click(setup);
        check(chooseMenuItem(1, 2), "settings menu: Quality submenu");
        check(juce::roundToInt(rawp(paramid::quality)) == 2, "settings: Quality High");
        click(setup);
        check(chooseMenuItem(2, 2), "settings menu: Analog variance submenu");
        check(std::abs(rawp(paramid::variance) - 0.5f) < 0.01f, "settings: variance 50 %");
        click(setup);
        check(chooseMenuItem(4, 1), "settings menu: Unit revision submenu");
        check(juce::roundToInt(rawp(paramid::revision)) == 1, "settings: revision Early");
        const auto seedBefore = processor->unitSeed();
        click(setup);
        check(chooseMenuItem(3, 0), "settings menu: Serial number submenu");
        check(processor->unitSeed() != seedBefore || processor->unitSeed() == 0, "settings: serial number randomised");
        click(setup);
        check(chooseMenuItem(5, 3), "settings menu: UI scale submenu");
        pump(60);
        check(std::abs(ed.scale() - 1.25f) < 0.01f && std::abs(processor->uiState().scale - 1.25f) < 0.01f, "settings: UI scale 125 %");
        ed.setScale(1.0f);
        pump(30);
        click(setup);
        check(chooseMenuItem(6, 0), "settings menu: MIDI submenu");
        check(rawp(paramid::velocitySens) < 0.5f, "settings: velocity sensitivity off");
        click(setup);
        check(chooseMenuItem(7), "settings menu: Timing jitter");
        check(rawp(paramid::jitter) > 0.5f, "settings: timing jitter on");
    }

    // --- session restore while open: the editor follows the restored UI state ---
    {
        juce::MemoryBlock state;
        auto ui = processor->uiState();
        ui.view = 1;
        ui.selectedTrack = 7;
        processor->setUiState(ui);
        processor->getStateInformation(state);
        ed.setView(ui::View::Panel);
        processor->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        pump(60);
        check(ed.view() == ui::View::Grid, "state restore with the editor open switches it to the stored view");
    }

    // --- gestures balanced ---
    pump(100);
    check(gestures.begins == gestures.ends, "every gesture begun was ended");
    std::printf("  gestures: %d begin / %d end\n", gestures.begins, gestures.ends);
    processor->removeListener(&gestures);
    return failures == 0 ? 0 : 1;
}

// --- lifetime: close the editor with menus, alerts and drags in flight ------------------------

int lifetime(int iterations) {
    using namespace mw909;
    auto processor = makeProcessor();
    setParam(*processor, paramid::seqMode, 1.0f);
    GestureCounter gestures;
    processor->addListener(&gestures);
    AudioThread audio(*processor, true);
    constexpr int kCases = 15;
    const bool dialogs = dialogToolInstalled();
    int choosersSeen = 0;
    for (int i = 0; i < iterations; ++i) {
        auto open = std::make_unique<OpenEditor>(*processor);
        if (open->editor == nullptr || open->peer == nullptr) { check(false, "editor + peer"); break; }
        auto& ed = *open->editor;
        Mouse mouse{open->peer};
        const int which = i % kCases;
        ed.setView(static_cast<ui::View>((i / kCases) % ui::kNumViews));
        if (which == 9) ed.setView(ui::View::Fx);
        if (which == 10) ed.setView(ui::View::Mixer);
        if (which == 12) ed.setView(ui::View::Panel);
        pump(20);
        auto* strip = findOne<ui::SequencerStrip>(ed);
        auto* header = findOne<ui::HeaderBar>(ed);
        switch (which) {
            case 0:  // parameter menu open
                if (auto* c = findControl(ed, "master_volume")) mouse.rightClick(*c);
                break;
            case 1:  // settings menu with a submenu open
                mouse.click(*findButton(*header, "SETUP"));
                pump(30);
                if (auto* m = topModal()) { m->keyPressed(juce::KeyPress(juce::KeyPress::downKey)); m->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)); }
                break;
            case 2: mouse.click(*findButton(*header, "SAVE")); break;  // save AlertWindow (desktop window)
            case 3: {  // knob mid-drag
                if (auto* c = findControl(ed, "tempo")) {
                    const auto p = mouse.at(*c);
                    mouse.press(p);
                    mouse.move(p.translated(0.0f, -20.0f), juce::ModifierKeys::leftButtonModifier);
                } else if (auto* k = findControl(ed, paramid::masterVolume)) {
                    const auto p = mouse.at(*k);
                    mouse.press(p);
                    mouse.move(p.translated(0.0f, -20.0f), juce::ModifierKeys::leftButtonModifier);
                }
                break;
            }
            case 4: {  // step key mid-paint
                juce::Array<ui::StepKey*> keys; collect(*strip, keys);
                mouse.press(mouse.at(*keys[0]));
                mouse.move(mouse.at(*keys[3]), juce::ModifierKeys::leftButtonModifier);
                break;
            }
            case 5: {  // external drag of the .mid in flight
                if (auto* h = findOne<ui::MidiDragHandle>(ed)) {
                    const auto p = mouse.at(*h);
                    mouse.press(p);
                    for (int k = 1; k <= 4; ++k) mouse.move(p.translated(0.0f, -5.0f * static_cast<float>(k)), juce::ModifierKeys::leftButtonModifier);
                }
                break;
            }
            case 6: mouse.click(*findButton(*header, "...")); break;  // preset menu
            case 7:  // serial number AlertWindow via the settings menu (submenu 3, "Enter...")
                mouse.click(*findButton(*header, "SETUP"));
                chooseMenuItem(3, 1);
                break;
            case 8: {  // song editor + scale value box menu
                ed.showSongEditor(true);
                mouse.click(*strip->scaleBox());
                break;
            }
            case 9: {  // FX slot mid-drag
                if (auto* fx = findOne<ui::FxView>(ed)) {
                    auto& rack = fx->rack();
                    const auto p = mouse.at(rack, rack.slotBounds(0, 0).getCentre().translated(0, 6));
                    mouse.press(p);
                    for (int k = 1; k <= 4; ++k) mouse.move(p.translated(15.0f * static_cast<float>(k), 10.0f * static_cast<float>(k)), juce::ModifierKeys::leftButtonModifier);
                }
                break;
            }
            case 10: {  // mixer strip expanded, a strip knob mid-drag, then an OUT menu
                if (auto* mixer = findOne<ui::MixerView>(ed)) {
                    mixer->setExpanded(i % kNumVoices);
                    pump(10);
                    if (auto* box = findBound<ui::ParamValueBox>(*mixer, "bd_out")) mouse.click(*box);
                }
                break;
            }
            case 11:  // About box
                mouse.click(*findButton(*header, "SETUP"));
                {
                    pump(30);
                    if (auto* m = topModal()) {
                        for (int k = 0; k < 9; ++k) m->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                        m->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
                    }
                }
                break;
            case 12: {  // LOAD (left click): the user-ROM file chooser
                juce::Array<juce::TextButton*> buttons; collect(ed, buttons);
                for (auto* b : buttons)
                    if (b->getButtonText() == "LOAD" && b->isShowing()) { mouse.click(*b); break; }
                break;
            }
            case 13:  // "..." menu: Import MIDI file (Save, [Delete: disabled on a factory preset], Rescan, Show, Import)
            case 14:  // ... and Export
                mouse.click(*findButton(*header, "..."));
                chooseMenuItem(which == 13 ? 4 : 5);
                break;
            default: break;
        }
        pump(15);
        if (which >= 12 && dialogs) {
            // The native dialog is a child process; give it a moment to start.
            for (int k = 0; k < 40 && liveDialogChildren() == 0; ++k) pump(25);
            const bool launched = liveDialogChildren() > 0;
            check(launched, "the file chooser opened (a dialog process runs)");
            choosersSeen += launched ? 1 : 0;
        }
        if (i % 4 == 0 && which < 12) processor->setCurrentProgram(i % juce::jmax(1, processor->getNumPrograms()));
        // Host closes the window now, whatever is open.
        open.reset();
        pump(30);
        // Nothing of ours may still be modal, and no file dialog may outlive the editor.
        check(topModal() == nullptr, "no modal window left behind");
        for (int k = 0; k < 20 && liveDialogChildren() > 0; ++k) pump(10);
        check(liveDialogChildren() == 0, "no file-chooser dialog process outlives the editor");
        if (failures > 0) { std::printf("  iteration %d (case %d)\n", i, which); break; }
    }
    juce::ModifierKeys::currentModifiers = {};
    pump(200);
    std::printf("  %d lifetime cycles, audio thread processed %ld blocks, gestures %d begin / %d end, %d file choosers closed with the editor%s\n",
                iterations, audio.blocks(), gestures.begins, gestures.ends, choosersSeen, dialogs ? "" : " (no zenity / kdialog: not exercised)");
    check(gestures.begins == gestures.ends, "no host gesture left open by a closed editor");
    processor->removeListener(&gestures);
    return failures == 0 ? 0 : 1;
}

// --- perf: what actually gets repainted while the sequencer runs -------------------------------

struct RepaintProbe final : juce::Component {
    RepaintProbe() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override {
        ++frames;
        // The clip is a rectangle list; sample it on an 8 px lattice for the real dirty area.
        auto& ctx = g.getInternalContext();
        const auto bounds = g.getClipBounds();
        long hit = 0, total = 0;
        for (int y = 0; y < getHeight(); y += 8)
            for (int x = 0; x < getWidth(); x += 8) {
                ++total;
                if (bounds.contains(x, y) && ctx.clipRegionIntersects({x, y, 1, 1})) ++hit;
            }
        const double frac = static_cast<double>(hit) / static_cast<double>(juce::jmax(1L, total));
        area += frac;
        maxFrac = juce::jmax(maxFrac, frac);
        if (frac > 0.9) ++fullFrames;
    }
    void reset() { frames = fullFrames = 0; area = maxFrac = 0.0; }
    long frames = 0, fullFrames = 0;
    double area = 0.0, maxFrac = 0.0;
};

int perf(int seconds) {
    using namespace mw909;
    auto processor = makeProcessor();
    setParam(*processor, paramid::seqMode, 1.0f);
    OpenEditor open(*processor);
    if (open.editor == nullptr) return 1;
    auto& ed = *open.editor;
    RepaintProbe probe;
    ed.addAndMakeVisible(probe);
    probe.setBounds(ed.getLocalBounds());
    // Stopped, nothing sounding: the editor should not paint at all.
    for (int v = 0; v < ui::kNumViews; ++v) {
        ed.setView(static_cast<ui::View>(v));
        settle(open.peer, 1500);  // let lamps fade and the meter fall
        probe.reset();
        pump(seconds * 1000);
        std::printf("  %-5s stopped: %.2f frames/s\n", ui::kViewNames[v], static_cast<double>(probe.frames) / seconds);
        check(probe.frames <= seconds, "stopped editor is (nearly) paint-free");
    }
    AudioThread audio(*processor, true);
    for (int v = 0; v < ui::kNumViews; ++v) {
        ed.setView(static_cast<ui::View>(v));
        settle(open.peer, 500);
        probe.reset();
        const double c0 = cpuSeconds();
        pump(seconds * 1000);
        const double used = cpuSeconds() - c0;
        std::printf("  %-5s playing: %.1f frames/s, mean dirty %.1f%% of the window per frame, max %.1f%%, %ld full repaints; CPU %.2f%% of a core (incl. DSP)\n",
                    ui::kViewNames[v], static_cast<double>(probe.frames) / seconds,
                    probe.frames > 0 ? probe.area / static_cast<double>(probe.frames) * 100.0 : 0.0, probe.maxFrac * 100.0, probe.fullFrames,
                    used / seconds * 100.0);
        check(probe.fullFrames == 0, "no full-window repaint while playing");
    }
    ed.removeChildComponent(&probe);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mw909_ui_tool screenshots <dir> | stress <n> | idle <seconds> | interact | wiring | lifetime <n> | perf <seconds>\n");
        return 2;
    }
    // JUCE installs its X error handler only in standalone JUCE apps; this is a
    // plain console app, and Xlib's default handler exits the process on the
    // first harmless BadAtom a bare Xvfb (no window manager) produces.
#if defined(__linux__)
    XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
#endif
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String cmd(argv[1]);
    int result = 2;
    std::printf("mw909_ui_tool %s\n", argv[1]);
    if (cmd == "screenshots" && argc >= 3) {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
        result = screenshots(dir);
        if (result == 0) result = states(dir);
    }
    else if (cmd == "stress") result = stress(argc >= 3 ? juce::String(argv[2]).getIntValue() : 200);
    else if (cmd == "idle") result = idle(argc >= 3 ? juce::jmax(1, juce::String(argv[2]).getIntValue()) : 5);
    else if (cmd == "interact") result = interact();
    else if (cmd == "wiring") result = wiring();
    else if (cmd == "lifetime") result = lifetime(argc >= 3 ? juce::String(argv[2]).getIntValue() : 96);
    else if (cmd == "perf") result = perf(argc >= 3 ? juce::jmax(1, juce::String(argv[2]).getIntValue()) : 5);
    else std::fprintf(stderr, "unknown command\n");
    juce::PopupMenu::dismissAllActiveMenus();
    pump(20);
    std::printf("%s: %s (%d failure%s)\n", argv[1], result == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return result;
}
