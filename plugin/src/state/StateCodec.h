// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The serialised form of the plugin state (docs/design/plugin.md §3) and of
// the preset files, as pure functions between plain data and a juce::ValueTree.
// Nothing here touches a processor, a parameter or a thread.
//
//   <mw909 stateVersion="1" program="0" unitSeed="909">
//     <PARAMETERS> <PARAM id="bd_tune" value="0.5"/> ... </PARAMETERS>
//     <sequencer patternFormat="1" patternSize="1566" songTrackSize="258"
//                selected="0" chainLength="0" [chain="base64"]>
//       <pattern index="0" data="base64 of the raw Pattern bytes"/> ...   (non-empty patterns only)
//       <track index="0" data="base64 of the raw SongTrack bytes"/> ...   (non-empty tracks only)
//     </sequencer>
//     <userRoms> <rom slot="0" name="..." data="base64 of the 32768 6-bit words"/> ... </userRoms>
//     <midiLearn> <map cc="9" param="swing_scale"/> ... </midiLearn>
//     <ui scale="1" view="0" track="0" stepMode="0" page="0" authentic="0"
//         kitName="" bankName="" presetName=""/>
//   </mw909>
//
// Pattern and song-track blobs are the raw bytes of the POD structs, tagged
// with their sizeof and seq::kPatternFormat; blobs that do not match are
// refused (mw808's patterns, for one, never load here).
//
// Parameter values are written as the shortest decimal that reads back to the
// same float, in layout order, so saving is deterministic and a load restores
// every value bit for bit. Reading never throws and never trusts a field: what
// cannot be read falls back to the defaults handed in.

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

#include "../MidiLearn.h"
#include "PatternModel.h"

namespace mw909::plugin::state {

inline constexpr int kStateVersion = 1;
inline constexpr int kPatternFormat = static_cast<int>(seq::kPatternFormat);

inline constexpr const char* kRootType = "mw909";
inline constexpr const char* kParametersType = "PARAMETERS";  // the APVTS state type
inline constexpr const char* kSequencerType = "sequencer";

// Everything a session holds.
struct PluginState {
    // (id, real-world value). After a read: one entry per known parameter, in
    // layout order.
    std::vector<std::pair<juce::String, float>> parameters;
    PatternModel::Snapshot model{};
    MidiLearn::Map midiMap{};
    int program = 0;
};

enum class ReadStatus {
    Ok,
    Corrupt,        // not a state tree at all
    FutureVersion   // written by a newer build; not interpreted
};

struct ReadResult {
    ReadStatus status = ReadStatus::Corrupt;
    std::unique_ptr<PluginState> state;  // set when status == Ok
};

// --- whole state -----------------------------------------------------------------
juce::ValueTree toTree(const PluginState& state);
// `defaults` supplies every value the tree does not hold or holds unreadably.
ReadResult fromTree(const juce::ValueTree& root, const PluginState& defaults);

// Upgrade a tree written with an older stateVersion to the current layout.
juce::ValueTree migrate(juce::ValueTree root, int fromVersion);

// --- parts (shared with the preset files) ------------------------------------------
juce::ValueTree parametersToTree(const std::vector<std::pair<juce::String, float>>& values);
// Values of the PARAM children that carry a readable finite number. Ids are not checked.
std::vector<std::pair<juce::String, float>> parametersFromTree(const juce::ValueTree& tree);

juce::ValueTree sequencerToTree(const seq::PatternBank& bank, int selectedPattern);
// False (outputs untouched) when the tree is not a sequencer tree this build
// can read. A single unreadable pattern or track blob leaves that one empty.
bool sequencerFromTree(const juce::ValueTree& tree, seq::PatternBank& bank, int& selectedPattern);

// User ROM slots: only the slots set in `mask` (bit = slot) are written.
juce::ValueTree userRomsToTree(const UserRoms& roms, unsigned mask = (1U << kNumRomSlots) - 1U);
// Slots the tree holds a readable image for are set in `roms` (and their bit
// in the result); unreadable or absent slots are left alone.
unsigned userRomsFromTree(const juce::ValueTree& tree, UserRoms& roms);
inline constexpr const char* kUserRomsType = "userRoms";

// Strict UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above
// U+10FFFF, no stray continuation bytes, no truncated sequence. Everything
// this plugin writes passes; JUCE's String and XML reader assert on (or
// mis-read) much of what fails, so such text is refused before it is parsed.
bool isValidUtf8(const char* text, std::size_t numBytes) noexcept;

// True when XML text carries a document type declaration. Nothing this plugin
// writes has one, and the XML parser would act on the entity definitions
// inside it (nested expansion, even reading other files), so text that does
// is refused without being parsed.
bool hasDoctype(const char* utf8, std::size_t numBytes) noexcept;

// True when XML text nests elements more than `maxDepth` deep. JUCE's XML
// reader (and ValueTree::fromXml after it) recurse once per level, so a
// crafted document a few hundred thousand levels deep would exhaust the stack
// of whatever thread loads it. Quoted attribute values, comments, CDATA and
// processing instructions are skipped, as the parser skips them.
bool xmlNestingExceeds(const char* utf8, std::size_t numBytes, int maxDepth) noexcept;

// Nothing this plugin writes nests deeper than 4 levels (a full preset wraps
// a state) or comes near 1 MB (96 patterns as base64 are about 200 kB, three
// user ROMs about 130 kB). Text beyond these limits is not ours and is refused
// before it is parsed.
inline constexpr int kMaxXmlDepth = 16;
inline constexpr std::size_t kMaxStateBytes = 2u * 1024u * 1024u;

// Shortest decimal text that reads back to exactly `value`.
juce::String floatToText(float value);
// False when the text is not a finite number.
bool textToFloat(const juce::String& text, float& value);

}  // namespace mw909::plugin::state
