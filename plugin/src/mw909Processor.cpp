// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "mw909Processor.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include <juce_audio_formats/juce_audio_formats.h>

#include "mw909/Factory.h"
#include "mw909/MidiMap.h"
#include "mw909/dsp/Rom.h"
#include "mw909/seq/MidiFile.h"

// ---------------------------------------------------------------------------
// Editor selection. CMake decides: -DMW909_BUILD_EDITOR=ON compiles the
// sources under plugin/ui and uses the real editor (plugin/ui/mw909Editor.h,
// class mw909Editor); OFF (the default until the editor lands) defines
// MW909_USE_PLACEHOLDER_EDITOR=1 and leaves plugin/ui out of the build.
#ifndef MW909_USE_PLACEHOLDER_EDITOR
 #define MW909_USE_PLACEHOLDER_EDITOR 1
#endif
// ---------------------------------------------------------------------------

#if MW909_USE_PLACEHOLDER_EDITOR
 #include "PlaceholderEditor.h"
using mw909EditorClass = mw909::plugin::PlaceholderEditor;
#else
 #include "mw909Editor.h"
using mw909EditorClass = mw909Editor;
#endif

#ifndef MW909_PRODUCT_NAME
 #define MW909_PRODUCT_NAME "mw909"
#endif

namespace {

using mw909::plugin::state::PluginState;
using mw909::plugin::state::ReadStatus;

bool onMessageThread() noexcept { return juce::MessageManager::existsAndIsCurrentThread(); }

// Holds the process gate for a scope, waiting for a block in flight to finish.
// Never used on the audio thread.
class GateLock {
public:
    explicit GateLock(std::atomic<bool>& gate) : gate_(gate) {
        while (gate_.exchange(true, std::memory_order_acquire)) std::this_thread::yield();
    }
    ~GateLock() { gate_.store(false, std::memory_order_release); }
    GateLock(const GateLock&) = delete;
    GateLock& operator=(const GateLock&) = delete;

private:
    std::atomic<bool>& gate_;
};

// A normalised value close to `normalised` whose real-world value the APVTS
// does not consider approximately equal to that of `normalised` (so setting it
// is not ignored), searching outwards from one part in a million.
float nearbyDistinctValue(const juce::RangedAudioParameter& p, float normalised) {
    const float target = p.convertFrom0to1(normalised);
    for (float delta = 1.0e-6f; delta <= 0.5f; delta *= 4.0f)
        for (const float sign : {1.0f, -1.0f}) {
            const float candidate = normalised + sign * delta;
            if (candidate < 0.0f || candidate > 1.0f) continue;
            if (!juce::approximatelyEqual(p.convertFrom0to1(candidate), target)) return candidate;
        }
    return normalised < 0.5f ? 1.0f : 0.0f;
}

// The audio thread's side: one attempt, no waiting.
class GateTry {
public:
    explicit GateTry(std::atomic<bool>& gate) noexcept
        : gate_(gate), entered_(!gate.exchange(true, std::memory_order_acquire)) {}
    ~GateTry() {
        if (entered_) gate_.store(false, std::memory_order_release);
    }
    GateTry(const GateTry&) = delete;
    GateTry& operator=(const GateTry&) = delete;
    bool entered() const noexcept { return entered_; }

private:
    std::atomic<bool>& gate_;
    bool entered_;
};

}  // namespace

juce::AudioProcessor::BusesProperties mw909Processor::makeBuses() {
    // One stereo main output, plus fifteen individual outs the host may enable.
    auto buses = BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int i = 2; i <= mw909::kNumBuses; ++i)
        buses = buses.withOutput("Out " + juce::String(i), juce::AudioChannelSet::stereo(), false);
    return buses;
}

mw909Processor::mw909Processor()
    : juce::AudioProcessor(makeBuses()),
      apvts_(*this, nullptr, juce::Identifier(mw909::plugin::state::kParametersType),
             mw909::plugin::createParameterLayout()),
      params_(apvts_),
      machine_(std::make_unique<mw909::Machine>()),
      patterns_(*machine_),
      midiLearn_(apvts_),
      presets_(*this) {
    for (const auto& id : mw909::plugin::allParameterIds()) {
        orderedParams_.push_back(apvts_.getParameter(juce::String(id)));
        orderedRaw_.push_back(apvts_.getRawParameterValue(juce::String(id)));
        jassert(orderedParams_.back() != nullptr && orderedRaw_.back() != nullptr);
    }

    midiExportDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(MW909_PRODUCT_NAME "-midi");

    // A new instance is factory preset 0. Nothing else can see the object yet,
    // so this applies directly whatever thread constructs the plugin.
    const auto defaults = makeDefaultState();
    for (std::size_t i = 0; i < orderedParams_.size(); ++i) {
        auto* p = orderedParams_[i];
        const float normalised = p->convertTo0to1(defaults->parameters[i].second);
        if (!juce::exactlyEqual(p->getValue(), normalised)) p->setValueNotifyingHost(normalised);
    }
    patterns_.initialise(std::make_unique<PatternModel::Snapshot>(defaults->model));

    startTimerHz(kServiceHz);
}

mw909Processor::~mw909Processor() { stopTimer(); }

const juce::String mw909Processor::getName() const { return MW909_PRODUCT_NAME; }

// --- audio setup -------------------------------------------------------------------

void mw909Processor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    int latency = 0;
    {
        const GateLock lock(processGate_);
        prepared_.store(false, std::memory_order_release);
        const double rate = (std::isfinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 44100.0;
        machine_->prepare(rate, std::max(1, samplesPerBlock));
        latency = machine_->latencySamples();
        // The worst case a block writes, reserved once here: processBlock swaps
        // this storage into a host buffer that came with less (the Standalone's).
        midiOutReserve_.clear();
        midiOutReserve_.ensureSize(kMidiOutReserveBytes);
        if (midiOutReserve_.data.begin() != midiOutStorage_) midiOutStorage_ = midiOutReserve_.data.begin();
        resetPending_.store(false, std::memory_order_release);
        numCarriedMidiIn_ = 0;
        midiOutFlushPending_.store(true, std::memory_order_release);
        prepared_.store(true, std::memory_order_release);
    }
    // Constant for a prepared sample rate; this is the only place it is set
    // (it tells the host, so outside the gate).
    setLatencySamples(latency);
}

void mw909Processor::releaseResources() {
    // Nothing to free: the Machine keeps its (small) scratch buffers, so a host
    // that processes after releasing still gets valid audio.
}

void mw909Processor::reset() {
    // May arrive on any thread; the next block silences the voices.
    resetPending_.store(true, std::memory_order_release);
}

bool mw909Processor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    for (const auto& in : layouts.inputBuses)
        if (!in.isDisabled()) return false;
    if (layouts.outputBuses.isEmpty()) return false;
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    for (int i = 1; i < layouts.outputBuses.size(); ++i) {
        const auto& set = layouts.outputBuses.getReference(i);
        if (!set.isDisabled() && set != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

// --- audio thread ------------------------------------------------------------------

void mw909Processor::makeSnapshot(mw909::MachineParams& out) const noexcept {
    params_.snapshot(out);
    out.unitSeed = patterns_.unitSeedForAudio();
}

mw909::seq::Transport mw909Processor::readTransport(double fallbackBpm) const noexcept {
    mw909::seq::Transport t;
    t.bpm = fallbackBpm;
    if (auto* head = getPlayHead()) {
        if (const auto position = head->getPosition()) {
            const auto ppq = position->getPpqPosition();
            if (ppq.hasValue() && std::isfinite(*ppq)) {
                t.hostValid = true;
                t.ppq = *ppq;
                t.playing = position->getIsPlaying();
                const auto bpm = position->getBpm();
                if (bpm.hasValue() && std::isfinite(*bpm) && *bpm > 0.0) t.bpm = *bpm;
            }
        }
    }
    return t;
}

void mw909Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Not prepared yet, or a prepareToPlay is in progress on another thread:
    // silence, and leave the Machine alone.
    const GateTry gate(processGate_);
    if (!gate.entered() || !prepared_.load(std::memory_order_acquire)) {
        buffer.clear();
        midi.clear();
        return;
    }

    // --- MIDI in: notes to the Machine, controllers to MIDI learn -------------
    // Notes that came with a zero-length block (the VST3 wrapper makes one
    // whenever the host sends events without audio) are still in midiIn_, at
    // offset 0: this block plays them first.
    int numMidiIn = numCarriedMidiIn_;
    numCarriedMidiIn_ = 0;
    const int lastSample = std::max(0, numSamples - 1);
    for (const auto meta : midi) {
        if (meta.numBytes != 3) continue;
        const std::uint8_t status = meta.data[0];
        const int type = status & 0xF0;
        if (type == 0xB0) {
            midiLearn_.handleController(meta.data[1] & 0x7F, meta.data[2] & 0x7F);
        } else if ((type == 0x90 || type == 0x80) && numMidiIn < kMaxMidiIn) {
            mw909::MidiMessage& m = midiIn_[static_cast<std::size_t>(numMidiIn++)];
            m.offset = std::clamp(meta.samplePosition, 0, lastSample);
            m.status = status;
            m.data1 = static_cast<std::uint8_t>(meta.data[1] & 0x7F);
            m.data2 = static_cast<std::uint8_t>(meta.data[2] & 0x7F);
        }
    }
    midi.clear();
    if (numSamples <= 0) {
        // Nothing to render (the Machine renders nothing either): keep the
        // notes for the next block rather than dropping them.
        numCarriedMidiIn_ = numMidiIn;
        return;
    }

    // --- output buses ------------------------------------------------------------
    // Enabled stereo buses map onto the Machine's buses; a bus the host left
    // disabled stays null and its voices fall back to the main mix.
    mw909::Machine::ProcessArgs args;
    constexpr int kTracked = 64;
    std::array<bool, kTracked> written{};
    const int busCount = std::min(getBusCount(false), mw909::kNumBuses);
    for (int b = 0; b < busCount; ++b) {
        const auto* bus = getBus(false, b);
        if (bus == nullptr || !bus->isEnabled() || bus->getNumberOfChannels() != 2) continue;
        const int left = getChannelIndexInProcessBlockBuffer(false, b, 0);
        const int right = getChannelIndexInProcessBlockBuffer(false, b, 1);
        if (left < 0 || right < 0 || left >= numChannels || right >= numChannels || left == right) continue;
        args.out.ch[static_cast<std::size_t>(b)] = {buffer.getWritePointer(left), buffer.getWritePointer(right)};
        if (left < kTracked) written[static_cast<std::size_t>(left)] = true;
        if (right < kTracked) written[static_cast<std::size_t>(right)] = true;
    }

    const auto& mainOut = args.out.ch[0];
    if (mainOut[0] == nullptr || mainOut[1] == nullptr) {
        // No usable main bus (a layout isBusesLayoutSupported rejects).
        buffer.clear();
        return;
    }
    // Whatever the Machine will not overwrite must not carry garbage.
    for (int ch = 0; ch < numChannels; ++ch)
        if (ch >= kTracked || !written[static_cast<std::size_t>(ch)]) buffer.clear(ch, 0, numSamples);

    // --- parameters and transport: one snapshot per block ---------------------------
    mw909::MachineParams params;
    makeSnapshot(params);

    args.transport = readTransport(params.seq.internalBpm);
    // Without a host transport (standalone, some offline hosts) Host Sync
    // would never start: it runs on the internal clock, so Run always works.
    if (!args.transport.hostValid && params.seq.mode == mw909::seq::Mode::HostSync)
        params.seq.mode = mw909::seq::Mode::Internal;
    // Tempo-synced effects follow the clock the sequencer follows.
    const bool hostClock = args.transport.hostValid && params.seq.mode == mw909::seq::Mode::HostSync;
    params.engine.bpm = hostClock ? args.transport.bpm : params.seq.internalBpm;

    if (resetPending_.exchange(false, std::memory_order_acq_rel)) machine_->reset();

    args.midiIn = midiIn_.data();
    args.numMidiIn = numMidiIn;
    args.midiOut = &midiOut_;
    args.numSamples = numSamples;
    machine_->process(args, params);

    // --- MIDI out -------------------------------------------------------------------
    // Written into the host's buffer. The block is capped at kMaxMidiOutNoteOns
    // note-ons, and a note-off is passed on only for a note the host was
    // actually sent, so a block never writes more than kMidiOutReserveBytes
    // and no note is ever left hanging. The VST3 and CLAP wrappers reserve
    // exactly that much and LV2 more. The Standalone's player reserves
    // nothing: its (empty) buffer swaps storage with the one prepareToPlay
    // reserved, the player keeps that storage from then on, and nothing is
    // allocated here.
    const bool flush = midiOutFlushPending_.exchange(false, std::memory_order_acq_rel);
    if (!flush && midiOut_.count == 0) return;
    // (JUCE does not expose a buffer's capacity, so the reserved storage is
    // recognised by its address.)
    if (midi.data.begin() != midiOutStorage_ && midiOutReserve_.data.begin() == midiOutStorage_) {
        midiOutReserve_.clear();
        midi.swapWith(midiOutReserve_);  // both empty: only the storage changes hands
    }
    midi.ensureSize(kMidiOutReserveBytes);  // a no-op for any buffer reserved as above
    // After a re-prepare every open note is closed at the top of the block, on
    // its own channel. (The Machine re-arms its own note-offs for the same
    // notes; those are then recognised as already sent and dropped.)
    if (flush) releaseMidiOut(midi, 0);
    int noteOns = 0;
    for (int i = 0; i < midiOut_.count; ++i) {
        const mw909::MidiMessage& m = midiOut_.messages[static_cast<std::size_t>(i)];
        const int type = m.status & 0xF0;
        const auto note = static_cast<std::size_t>(m.data1 & 0x7F);
        const auto channelBit = static_cast<std::uint16_t>(1U << (m.status & 0x0F));
        if (type == 0x90 && m.data2 > 0) {
            if (noteOns >= kMaxMidiOutNoteOns) continue;
            ++noteOns;
            midiOutSounding_[note] = static_cast<std::uint16_t>(midiOutSounding_[note] | channelBit);
        } else if (type == 0x80 || type == 0x90) {
            // Only for a note the host was sent on this channel and that is
            // still open (a flush may have closed it already).
            if ((midiOutSounding_[note] & channelBit) == 0) continue;
            midiOutSounding_[note] = static_cast<std::uint16_t>(midiOutSounding_[note] & ~channelBit);
        } else {
            continue;
        }
        const std::uint8_t bytes[3] = {m.status, m.data1, m.data2};
        midi.addEvent(bytes, 3, std::clamp(static_cast<int>(m.offset), 0, lastSample));
    }
}

void mw909Processor::releaseMidiOut(juce::MidiBuffer& midi, int sample) noexcept {
    // Each note is closed on the channel it went out on (MIDI Out's channel
    // may have changed since). The Machine's own later note-offs for these
    // notes are then dropped in processBlock, since they are no longer open.
    for (std::size_t note = 0; note < midiOutSounding_.size(); ++note) {
        if (midiOutSounding_[note] == 0) continue;
        for (unsigned channel = 0; channel < 16; ++channel)
            if ((midiOutSounding_[note] & (1U << channel)) != 0) {
                const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(0x80 | channel),
                                               static_cast<std::uint8_t>(note), 0};
                midi.addEvent(bytes, 3, sample);
            }
        midiOutSounding_[note] = 0;
    }
}

void mw909Processor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    // Silence, MIDI passes through (as JUCE's default), plus a note-off for every
    // note MIDI Out sent and has not released: they would hang for as long as
    // the bypass lasts. The Machine's own late note-offs for them are dropped
    // after the bypass, since those notes are no longer marked sounding.
    // An instrument with no inputs: bypassed is silence, which needs no
    // latency matching (JUCE's default asserts when latency is reported).
    buffer.clear();
    const GateTry gate(processGate_);
    if (!gate.entered()) return;
    bool any = false;
    for (const std::uint16_t channels : midiOutSounding_) any = any || channels != 0;
    if (!any) return;
    // The host's MIDI input is still in the buffer (passed through).
    midi.ensureSize(static_cast<std::size_t>(midi.data.size()) + kMidiOutReserveBytes);
    releaseMidiOut(midi, 0);
}

// --- editor --------------------------------------------------------------------------

juce::AudioProcessorEditor* mw909Processor::createEditor() { return new mw909EditorClass(*this); }

// --- programs ------------------------------------------------------------------------

int mw909Processor::getNumPrograms() { return std::max(1, mw909::factory::numPresets()); }

int mw909Processor::getCurrentProgram() { return currentProgram_.load(std::memory_order_relaxed); }

const juce::String mw909Processor::getProgramName(int index) {
    if (index < 0 || index >= mw909::factory::numPresets()) return {};
    return juce::String(std::string(mw909::factory::preset(index).name));
}

void mw909Processor::changeProgramName(int, const juce::String&) {
    // Factory programs are read-only.
}

void mw909Processor::setCurrentProgram(int index) {
    // Hosts probe with bad indices: a no-op. Selecting the current program
    // again reloads it (an LV2 host's preset pick, a host program menu): the
    // JUCE 8 VST3 wrapper already filters its own redundant re-sends, and no
    // other wrapper here sends one.
    if (index < 0 || index >= getNumPrograms()) return;
    currentProgram_.store(index, std::memory_order_release);  // what the host reads back at once

    if (onMessageThread()) {
        pendingProgram_.store(-1, std::memory_order_release);
        presets_.load(mw909::plugin::PresetKind::Full, index);
    } else {
        // The VST3 wrapper delivers program changes on the audio thread.
        // Remember the request; the service timer loads it.
        pendingProgram_.store(index, std::memory_order_release);
    }
}

void mw909Processor::applyPendingProgram() {
    const int pending = pendingProgram_.exchange(-1, std::memory_order_acq_rel);
    if (pending >= 0 && pending < mw909::factory::numPresets()) presets_.load(mw909::plugin::PresetKind::Full, pending);
}

void mw909Processor::factoryProgramLoaded(int index) {
    if (index < 0 || index >= getNumPrograms()) return;
    appliedProgram_.store(index, std::memory_order_release);
    if (currentProgram_.exchange(index, std::memory_order_acq_rel) != index)
        programNotifyPending_.store(true, std::memory_order_release);
}

// --- state ---------------------------------------------------------------------------

std::unique_ptr<PluginState> mw909Processor::makeDefaultState() const {
    auto state = std::make_unique<PluginState>();
    state->parameters.reserve(orderedParams_.size());
    const auto& ids = mw909::plugin::allParameterIds();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto* p = orderedParams_[i];
        state->parameters.emplace_back(juce::String(ids[i]), p->convertFrom0to1(p->getDefaultValue()));
    }

    const auto& preset = mw909::factory::preset(0);
    const auto set = [&state](const std::string& id, float value) {
        const int i = mw909::plugin::parameterIndex(id);
        if (i >= 0) state->parameters[static_cast<std::size_t>(i)].second = value;
    };
    for (const auto& [id, value] : mw909::plugin::Parameters::kitValues(mw909::factory::kit(preset.kit))) set(id, value);
    for (const auto& [id, value] : mw909::plugin::Parameters::presetValues(preset)) set(id, value);

    mw909::factory::buildBank(preset.bank, state->model.bank);
    state->model.selectedPattern = std::clamp(preset.pattern, 0, mw909::seq::kNumPatterns - 1);
    state->model.unitSeed = mw909::plugin::kDefaultUnitSeed;
    state->model.roms = mw909::plugin::UserRoms{};
    state->midiMap = MidiLearn::factoryMap();
    state->model.ui = UiState{};
    state->model.ui.presetNames[static_cast<std::size_t>(mw909::plugin::PresetKind::Kit)] =
        juce::String(std::string(mw909::factory::kit(preset.kit).name));
    state->model.ui.presetNames[static_cast<std::size_t>(mw909::plugin::PresetKind::Patterns)] =
        juce::String(std::string(mw909::factory::bankInfo(preset.bank).name));
    state->model.ui.presetNames[static_cast<std::size_t>(mw909::plugin::PresetKind::Full)] =
        juce::String(std::string(preset.name));
    state->program = 0;
    return state;
}

std::unique_ptr<PluginState> mw909Processor::captureState() const {
    auto state = std::make_unique<PluginState>();
    const auto& ids = mw909::plugin::allParameterIds();
    state->parameters.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
        state->parameters.emplace_back(juce::String(ids[i]), orderedRaw_[i]->load(std::memory_order_relaxed));
    patterns_.capture(state->model);
    state->midiMap = midiLearn_.getMap();
    // The program the saved parameters belong to: a program change still
    // waiting for the timer has not reached them yet.
    state->program = appliedProgram_.load(std::memory_order_acquire);
    return state;
}

void mw909Processor::applyState(const PluginState& state, bool sessionExtras) {
    // Parameters: a complete tree in layout order, every value forced into its
    // parameter's legal set, handed to the APVTS in one piece.
    juce::ValueTree tree{juce::Identifier(mw909::plugin::state::kParametersType)};
    const auto& ids = mw909::plugin::allParameterIds();
    std::vector<float> targets(ids.size(), 0.0f);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto* p = orderedParams_[i];
        float value = p->convertFrom0to1(p->getDefaultValue());
        const juce::String id(ids[i]);
        // States from fromTree() and captureState() hold every parameter in
        // layout order: look there first, search only otherwise.
        const float* stored = nullptr;
        if (i < state.parameters.size() && state.parameters[i].first == id) {
            stored = &state.parameters[i].second;
        } else {
            for (const auto& [sid, svalue] : state.parameters)
                if (sid == id) {
                    stored = &svalue;
                    break;
                }
        }
        if (stored != nullptr && std::isfinite(*stored)) value = p->convertFrom0to1(p->convertTo0to1(*stored));
        targets[i] = value;
        juce::ValueTree child{juce::Identifier("PARAM")};
        child.setProperty("id", id, nullptr);
        child.setProperty("value", value, nullptr);
        tree.appendChild(child, nullptr);
    }
    // From here on the tree belongs to the APVTS (a ValueTree is a shared
    // reference): it is not touched again on this thread.
    apvts_.replaceState(tree);

    // The APVTS skips a value it considers "approximately equal" to the one it
    // holds (within a float epsilon). A restore has to be exact, so the rare
    // parameter left one rounding step off is moved away and back. "Away" is
    // the nearest value the APVTS will not ignore: the audio thread may run a
    // block in between, and a jump to the far end of the range (full master
    // volume, 300 BPM) would be heard.
    // The parameter's own value (what the host reads back) can be one rounding
    // step off while the APVTS copy is exact: setting it directly is enough.
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto* p = orderedParams_[i];
        const float normalised = p->convertTo0to1(targets[i]);
        if (!juce::exactlyEqual(orderedRaw_[i]->load(std::memory_order_relaxed), targets[i])) {
            p->setValueNotifyingHost(nearbyDistinctValue(*p, normalised));
            p->setValueNotifyingHost(normalised);
        } else if (!juce::exactlyEqual(p->getValue(), normalised)) {
            p->setValueNotifyingHost(normalised);
        }
    }

    patterns_.restore(std::make_unique<PatternModel::Snapshot>(state.model), !sessionExtras);
    if (sessionExtras) {
        midiLearn_.setMap(state.midiMap);
        const int program = std::clamp(state.program, 0, getNumPrograms() - 1);
        appliedProgram_.store(program, std::memory_order_release);
        currentProgram_.store(program, std::memory_order_release);
    }
}

void mw909Processor::getStateInformation(juce::MemoryBlock& destData) {
    // A program change still waiting for the timer belongs in the saved state.
    if (onMessageThread()) applyPendingProgram();

    const auto state = captureState();
    if (const auto xml = mw909::plugin::state::toTree(*state).createXml()) copyXmlToBinary(*xml, destData);
}

void mw909Processor::setStateInformation(const void* data, int sizeInBytes) {
    // The state being loaded supersedes any program change not applied yet.
    pendingProgram_.store(-1, std::memory_order_release);

    const auto defaults = makeDefaultState();
    mw909::plugin::state::ReadResult result;
    if (data != nullptr && sizeInBytes > 8) {
        // The block is JUCE's XML container: magic, text length, text. The XML
        // parser accepts a document that simply stops between two elements, so
        // a truncated block is caught here, by its declared length, rather
        // than half-loaded. The text must also be valid UTF-8 (everything this
        // plugin writes is): getXmlFromBinary asserts on anything else.
        const auto declared = static_cast<juce::int64>(
            juce::ByteOrder::littleEndianInt(juce::addBytesToPointer(data, 4)));
        const auto* text = static_cast<const char*>(juce::addBytesToPointer(data, 8));
        // Nesting and size are bounded before parsing too (see StateCodec.h).
        namespace st = mw909::plugin::state;
        if (declared > 0 && declared <= static_cast<juce::int64>(sizeInBytes) - 8 &&
            declared <= static_cast<juce::int64>(st::kMaxStateBytes) &&
            st::isValidUtf8(text, static_cast<std::size_t>(declared)) &&
            !st::hasDoctype(text, static_cast<std::size_t>(declared)) &&
            !st::xmlNestingExceeds(text, static_cast<std::size_t>(declared), st::kMaxXmlDepth)) {
            if (const auto xml = getXmlFromBinary(data, sizeInBytes))
                result = mw909::plugin::state::fromTree(juce::ValueTree::fromXml(*xml), *defaults);
        }
    }
    lastLoadStatus_.store(result.status, std::memory_order_release);

    // Unreadable or from a newer build: a known-good state rather than a
    // half-applied one.
    applyState(result.state != nullptr ? *result.state : *defaults, true);

    // CLAP: a state load changes parameter values behind the host's back, and
    // the host has to be asked to read them again. The CLAP wrapper turns
    // "program changed" into exactly that values rescan. The other wrappers
    // resynchronise the host themselves after a state load.
    if (isClap()) {
        if (onMessageThread())
            updateHostDisplay(ChangeDetails().withProgramChanged(true));
        else
            programNotifyPending_.store(true, std::memory_order_release);
    }
}

bool mw909Processor::isClap() const noexcept {
#if defined(HAS_CLAP_JUCE_EXTENSIONS)
    return clapProperties_.is_clap;
#else
    return false;
#endif
}

void mw909Processor::setParameterValues(const std::vector<std::pair<juce::String, float>>& values,
                                        bool withGestures) {
    for (const auto& [id, value] : values) {
        auto* p = apvts_.getParameter(id);
        if (p == nullptr || !std::isfinite(value)) continue;
        const float normalised = p->convertTo0to1(value);
        if (juce::exactlyEqual(p->getValue(), normalised)) continue;
        if (withGestures) p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        if (withGestures) p->endChangeGesture();
    }
}

// --- message-thread service ----------------------------------------------------------

void mw909Processor::timerCallback() { serviceMessageThread(); }

void mw909Processor::serviceMessageThread() {
    applyPendingProgram();
    patterns_.service();
    midiLearn_.service();

    if (panicPending_.load(std::memory_order_acquire)) {
        mw909::Command c;
        c.type = mw909::Command::Type::Panic;
        if (patterns_.postCommand(c)) panicPending_.store(false, std::memory_order_release);
    }

    const bool patternsDirty = patterns_.takeSessionDirty();
    const bool midiDirty = midiLearn_.takeSessionDirty();
    const bool programChanged = programNotifyPending_.exchange(false, std::memory_order_acq_rel);
    if (patternsDirty || midiDirty || programChanged)
        updateHostDisplay(ChangeDetails()
                              .withNonParameterStateChanged(patternsDirty || midiDirty)
                              .withProgramChanged(programChanged));
}

// --- editor API ----------------------------------------------------------------------

void mw909Processor::audition(mw909::VoiceId voice, bool accent) {
    if (static_cast<int>(voice) >= mw909::kNumVoices) return;
    mw909::Command c;
    c.type = mw909::Command::Type::Audition;
    c.index = static_cast<std::int32_t>(voice);
    const float totalAccent = apvts_.getRawParameterValue(mw909::plugin::paramid::totalAccent)->load();
    c.value = accent ? mw909::Hit::accented(true, true, totalAccent).acc : mw909::Hit::normal().acc;
    patterns_.postCommand(c);
}

void mw909Processor::requestFill() {
    mw909::Command c;
    c.type = mw909::Command::Type::Fill;
    patterns_.postCommand(c);
}

void mw909Processor::panic() {
    mw909::Command c;
    c.type = mw909::Command::Type::Panic;
    // A full queue must not swallow a panic: the service timer retries.
    if (!patterns_.postCommand(c)) panicPending_.store(true, std::memory_order_release);
}

juce::File mw909Processor::exportPatternToMidiFile(int patternIndex, int variation) {
    if (patternIndex < 0 || patternIndex >= mw909::seq::kNumPatterns) return {};
    const int var = std::clamp(variation, 0, mw909::seq::kNumVariations - 1);

    mw909::seq::MidiFileOptions options;
    options.bpm = static_cast<double>(apvts_.getRawParameterValue(mw909::plugin::paramid::tempo)->load());
    options.swingScale = apvts_.getRawParameterValue(mw909::plugin::paramid::swingScale)->load();
    const std::vector<std::uint8_t> bytes =
        mw909::seq::writeMidiFile(patterns_.patternCopy(patternIndex), var, mw909::kVoiceNote, options);
    if (bytes.empty()) return {};

    if (!midiExportDir_.createDirectory().wasOk()) return {};
    // "mw909 I-2-05 A.mid": bank I/II, group 1-3, pattern 1-16, variation.
    const juce::String bank = mw909::seq::bankOf(patternIndex) == 0 ? "I" : "II";
    const juce::String group(mw909::seq::groupOf(patternIndex) + 1);
    const juce::String slot = juce::String(mw909::seq::slotOf(patternIndex) + 1).paddedLeft('0', 2);
    const juce::File file = midiExportDir_.getChildFile(juce::String(MW909_PRODUCT_NAME) + " " + bank + "-" + group +
                                                        "-" + slot + (var == 0 ? " A" : " B") + ".mid");
    if (!file.replaceWithData(bytes.data(), bytes.size())) return {};
    return file;
}

bool mw909Processor::importMidiFile(const juce::File& file, int patternIndex) {
    return importMidiFile(file, patternIndex, 0);
}

bool mw909Processor::importMidiFile(const juce::File& file, int patternIndex, int variation) {
    if (patternIndex < 0 || patternIndex >= mw909::seq::kNumPatterns) return false;
    constexpr juce::int64 kMaxBytes = 4 * 1024 * 1024;
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > kMaxBytes) return false;

    juce::MemoryBlock data;
    if (!file.loadFileAsData(data) || data.getSize() == 0) return false;
    const auto* begin = static_cast<const std::uint8_t*>(data.getData());
    const std::vector<std::uint8_t> bytes(begin, begin + data.getSize());

    // Read into the pattern as the session holds it (a restore another thread
    // staged included), in one edit.
    const int var = std::clamp(variation, 0, mw909::seq::kNumVariations - 1);
    bool read = false;
    patterns_.edit(patternIndex, [&](mw909::seq::Pattern& pattern) {
        read = mw909::seq::readMidiFile(bytes, mw909::kVoiceNote, pattern, var);
        return read;
    });
    return read;
}

bool mw909Processor::importUserRom(const juce::File& audioFile, mw909::RomRegion region) {
    const int r = static_cast<int>(region);
    if (r < 0 || r > static_cast<int>(mw909::RomRegion::Ride)) return false;
    constexpr juce::int64 kMaxFileBytes = 256 * 1024 * 1024;
    if (!audioFile.existsAsFile() || audioFile.getSize() <= 0 || audioFile.getSize() > kMaxFileBytes) return false;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
    if (reader == nullptr || !std::isfinite(reader->sampleRate) || reader->sampleRate < 1000.0 ||
        reader->sampleRate > 1.0e6 || reader->numChannels < 1 || reader->lengthInSamples <= 0)
        return false;

    // More than the longest region holds (24576 words at ~32 kHz is under a
    // second) is never used; read a bounded head of the file.
    const auto maxSamples = static_cast<juce::int64>(reader->sampleRate * kMaxRomImportSeconds);
    const int numSamples = static_cast<int>(std::min(reader->lengthInSamples, maxSamples));
    const int numChannels = static_cast<int>(std::min<unsigned int>(reader->numChannels, 8U));
    juce::AudioBuffer<float> audio(numChannels, numSamples);
    if (!reader->read(&audio, 0, numSamples, 0, true, numChannels > 1)) return false;

    std::vector<float> mono(static_cast<std::size_t>(numSamples), 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::add(mono.data(), audio.getReadPointer(ch), numSamples);
    juce::FloatVectorOperations::multiply(mono.data(), 1.0f / static_cast<float>(numChannels), numSamples);
    for (float& x : mono)
        if (!std::isfinite(x)) x = 0.0f;

    const mw909::RomSlot slot = region == mw909::RomRegion::Crash  ? mw909::RomSlot::Crash
                                : region == mw909::RomRegion::Ride ? mw909::RomSlot::Ride
                                                                   : mw909::RomSlot::Hat;
    // Into a copy of what the slot plays now, so importing one hat region
    // keeps the other.
    const auto current = patterns_.userRom(slot);
    auto image = std::make_unique<mw909::RomImage>(current.image != nullptr ? *current.image
                                                                            : mw909::factoryRom(slot));
    if (!mw909::romFromAudio(mono.data(), numSamples, reader->sampleRate, region, *image)) return false;
    return patterns_.setUserRom(slot, *image, audioFile.getFileName());
}

void mw909Processor::clearUserRom(mw909::RomSlot slot) { patterns_.clearUserRom(slot); }

// The plugin entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new mw909Processor(); }
