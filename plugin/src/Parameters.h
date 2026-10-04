// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The APVTS parameter layout (docs/design/plugin.md §2) and the bridge that
// turns the cached parameter atomics into one sanitised mw909::MachineParams
// per block.
//
// Every parameter is one row of a table (Parameters.cpp): its id, name, kind,
// range and two plain accessors, one that writes a value into MachineParams
// and one that reads it back. The defaults, the per-block snapshot, the kit
// values of a factory kit and the layout itself all come from that one table,
// so adding a parameter is one row.

#pragma once

#include <array>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterIDs.h"
#include "mw909/Factory.h"
#include "mw909/Machine.h"

namespace mw909::plugin {

// A bool parameter whose host-visible value is always exactly 0 or 1.
//
// The stock AudioParameterBool keeps whatever normalised value the host poked
// (0.37 reads back as 0.37 although the parameter is "off"), so a host that
// saves, pokes and restores sees a different number come back. Reporting the
// snapped value keeps state save/restore exact from the host's point of view
// (clap-validator and pluginval both check this).
class BoolParameter final : public juce::AudioParameterBool {
public:
    using juce::AudioParameterBool::AudioParameterBool;

private:
    float getValue() const override { return get() ? 1.0f : 0.0f; }
};

enum class ParamKind : std::uint8_t { Float, Toggle, Choice, Int };  // (no "Bool": X11 #defines it)

// How a float parameter spreads over the normalised 0..1 range.
enum class ParamMap : std::uint8_t {
    Linear,
    Log,    // equal ratios per step (frequencies, times); low must be > 0
    Cubic   // low + (high - low) * n^3: a log-like taper that reaches 0
};

// One parameter.
struct ParamDef {
    std::string id;
    std::string name;
    ParamKind kind = ParamKind::Float;
    float low = 0.0f;
    float high = 1.0f;
    float def = 0.0f;                 // real-world default (derived from defaultMachineParams())
    double step = 0.0;                // Float: snapping grid, 0 = continuous
    ParamMap map = ParamMap::Linear;
    std::vector<std::string> choices; // Choice: labels, index = value
    std::string label;                // unit shown by hosts ("dB", "Hz", ...)
    int decimals = 2;
    bool automatable = true;
    bool kit = false;                 // part of a kit preset
    std::string groupId;              // host parameter group
    std::string groupName;
    int index = 0;                    // argument for the accessors (voice, bus, band, ...)
    void (*set)(MachineParams&, int index, float value) = nullptr;  // value already sanitised
    float (*get)(const MachineParams&, int index) = nullptr;
};

// Every parameter, in host (layout) order. This is also the canonical order
// of the serialised state.
const std::vector<ParamDef>& parameterDefs();
const std::vector<std::string>& allParameterIds();
// Layout index of an id, or -1.
int parameterIndex(const std::string& id);

// The parameters a kit preset carries: every per-voice sound, mixer and strip
// parameter except mute and solo, the FX section, the trigger outs, total
// accent, the master section and the unit revision (factory::Kit's fields).
const std::vector<std::string>& kitParameterIds();

// The engine as the parameters describe it at their defaults (plugin.md §2).
// Everything that is not a parameter (unit seed, bpm, jitter seed) is left at
// its MachineParams default.
MachineParams defaultMachineParams();

// A range whose normalise / denormalise pair round-trips every value the
// parameter can hold, bit for bit (computed in double; a value whose rounded
// image misses is found among its float neighbours). `step` > 0 snaps to that
// decimal grid. State restore relies on it.
juce::NormalisableRange<float> makeExactRange(float low, float high, double step = 0.0,
                                              ParamMap map = ParamMap::Linear);

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// Cached raw-value atomics of every parameter. Construction (message thread)
// looks them up once; snapshot() is plain relaxed loads and safe on the audio
// thread.
class Parameters {
public:
    explicit Parameters(juce::AudioProcessorValueTreeState& apvts);

    // Fills every parameter-backed field of `out` (sanitised: non-finite ->
    // default, clamped, switches and choices rounded). Leaves the rest alone
    // (unitSeed, engine.bpm, seq.jitterSeed).
    void snapshot(MachineParams& out) const noexcept;

    // (id, real-world value) of every kit parameter of a factory kit. An
    // effect instance the kit leaves off is reported in its default slot.
    static std::vector<std::pair<std::string, float>> kitValues(const factory::Kit& kit);
    // The sequencer settings a factory full preset brings (tempo, shuffle
    // depth, variation, play mode, song track, auto fill).
    static std::vector<std::pair<std::string, float>> presetValues(const factory::Preset& preset);

private:
    std::vector<std::atomic<float>*> raw_;  // layout order
};

}  // namespace mw909::plugin
