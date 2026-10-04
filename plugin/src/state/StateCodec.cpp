// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "StateCodec.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace mw909::plugin::state {

namespace {

const juce::Identifier kStateVersionProp{"stateVersion"};
const juce::Identifier kProgramProp{"program"};
const juce::Identifier kUnitSeedProp{"unitSeed"};

const juce::Identifier kParamType{"PARAM"};
const juce::Identifier kIdProp{"id"};
const juce::Identifier kValueProp{"value"};

const juce::Identifier kPatternFormatProp{"patternFormat"};
const juce::Identifier kPatternSizeProp{"patternSize"};
const juce::Identifier kSongTrackSizeProp{"songTrackSize"};
const juce::Identifier kTrackType{"track"};
const juce::Identifier kRomType{"rom"};
const juce::Identifier kSlotProp{"slot"};
const juce::Identifier kNameProp{"name"};
const juce::Identifier kSelectedProp{"selected"};
const juce::Identifier kChainLengthProp{"chainLength"};
const juce::Identifier kChainProp{"chain"};
const juce::Identifier kPatternType{"pattern"};
const juce::Identifier kIndexProp{"index"};
const juce::Identifier kDataProp{"data"};

const juce::Identifier kMidiLearnType{"midiLearn"};
const juce::Identifier kMapType{"map"};
const juce::Identifier kCcProp{"cc"};
const juce::Identifier kParamProp{"param"};

const juce::Identifier kUiType{"ui"};
const juce::Identifier kScaleProp{"scale"};
const juce::Identifier kViewProp{"view"};
const juce::Identifier kTrackProp{"track"};
const juce::Identifier kStepModeProp{"stepMode"};
const juce::Identifier kPageProp{"page"};
const juce::Identifier kAuthenticProp{"authentic"};
const std::array<juce::Identifier, 3> kPresetNameProps{juce::Identifier{"kitName"}, juce::Identifier{"bankName"},
                                                       juce::Identifier{"presetName"}};

// Integer property with range check; `fallback` when missing, not a number or out of range.
int intProperty(const juce::ValueTree& tree, const juce::Identifier& name, int low, int high, int fallback) {
    if (!tree.hasProperty(name)) return fallback;
    const juce::String text = tree.getProperty(name).toString().trim();
    if (text.isEmpty() || !text.containsOnly("-0123456789") || text.length() > 11) return fallback;
    const juce::int64 value = text.getLargeIntValue();
    if (value < low || value > high) return fallback;
    return static_cast<int>(value);
}

bool decodeBlob(const juce::String& text, void* dest, std::size_t size) {
    // Base64 of `size` bytes is at most 4 * ceil(size / 3) characters.
    if (text.isEmpty() || static_cast<std::size_t>(text.length()) > 4 * ((size + 2) / 3)) return false;
    juce::MemoryOutputStream out;
    if (!juce::Base64::convertFromBase64(out, text)) return false;
    if (out.getDataSize() != size) return false;
    std::memcpy(dest, out.getData(), size);
    return true;
}

}  // namespace

bool isValidUtf8(const char* text, std::size_t numBytes) noexcept {
    if (text == nullptr) return numBytes == 0;
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    std::size_t i = 0;
    while (i < numBytes) {
        const unsigned c = s[i];
        if (c < 0x80U) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned low = 0x80U, high = 0xBFU;  // allowed range of the second byte
        if (c >= 0xC2U && c <= 0xDFU) {
            extra = 1;
        } else if (c == 0xE0U) {
            extra = 2;
            low = 0xA0U;  // no overlong 3-byte forms
        } else if (c >= 0xE1U && c <= 0xECU) {
            extra = 2;
        } else if (c == 0xEDU) {
            extra = 2;
            high = 0x9FU;  // no surrogates
        } else if (c >= 0xEEU && c <= 0xEFU) {
            extra = 2;
        } else if (c == 0xF0U) {
            extra = 3;
            low = 0x90U;  // no overlong 4-byte forms
        } else if (c >= 0xF1U && c <= 0xF3U) {
            extra = 3;
        } else if (c == 0xF4U) {
            extra = 3;
            high = 0x8FU;  // nothing above U+10FFFF
        } else {
            return false;  // continuation byte, overlong lead, or 5/6-byte form
        }
        if (i + extra >= numBytes) return false;  // truncated sequence
        const unsigned second = s[i + 1];
        if (second < low || second > high) return false;
        for (std::size_t k = 2; k <= extra; ++k)
            if ((s[i + k] & 0xC0U) != 0x80U) return false;
        i += extra + 1;
    }
    return true;
}

bool hasDoctype(const char* utf8, std::size_t numBytes) noexcept {
    static constexpr char kNeedle[] = "<!doctype";
    constexpr std::size_t kLength = sizeof(kNeedle) - 1;
    if (utf8 == nullptr || numBytes < kLength) return false;
    for (std::size_t i = 0; i + kLength <= numBytes; ++i) {
        if (utf8[i] != '<') continue;
        std::size_t k = 1;
        while (k < kLength) {
            const char c = utf8[i + k];
            const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (lower != kNeedle[k]) break;
            ++k;
        }
        if (k == kLength) return true;
    }
    return false;
}

bool xmlNestingExceeds(const char* utf8, std::size_t numBytes, int maxDepth) noexcept {
    if (utf8 == nullptr) return false;
    const auto startsWith = [&](std::size_t at, const char* what) {
        const std::size_t n = std::strlen(what);
        return at + n <= numBytes && std::memcmp(utf8 + at, what, n) == 0;
    };
    // Index just past `terminator` searched from `from`, or numBytes when absent.
    const auto skipPast = [&](std::size_t from, const char* terminator) {
        const std::size_t n = std::strlen(terminator);
        for (std::size_t i = from; i + n <= numBytes; ++i)
            if (std::memcmp(utf8 + i, terminator, n) == 0) return i + n;
        return numBytes;
    };

    int depth = 0;
    std::size_t i = 0;
    while (i < numBytes) {
        if (utf8[i] != '<') {
            ++i;
            continue;
        }
        if (startsWith(i, "<!--")) {
            i = skipPast(i + 4, "-->");
        } else if (startsWith(i, "<![CDATA[")) {
            i = skipPast(i + 9, "]]>");
        } else if (startsWith(i, "<?")) {
            i = skipPast(i + 2, "?>");
        } else if (startsWith(i, "<!")) {
            i = skipPast(i + 2, ">");
        } else if (startsWith(i, "</")) {
            i = skipPast(i + 2, ">");
            if (depth > 0) --depth;
        } else {
            // An opening tag: find its end outside quoted attribute values.
            std::size_t j = i + 1;
            char quote = 0;
            bool selfClosing = false;
            for (; j < numBytes; ++j) {
                const char c = utf8[j];
                if (quote != 0) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '>') {
                    selfClosing = utf8[j - 1] == '/';
                    break;
                }
            }
            i = j + 1;
            if (!selfClosing && ++depth > maxDepth) return true;
        }
    }
    return false;
}

juce::String floatToText(float value) {
    if (!std::isfinite(value)) return "0";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return juce::String(juce::CharPointer_UTF8(buffer), juce::CharPointer_UTF8(result.ptr));
}

bool textToFloat(const juce::String& text, float& value) {
    const std::string s = text.trim().toStdString();
    if (s.empty() || s.size() > 64) return false;
    float parsed = 0.0f;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (result.ec != std::errc() || result.ptr != s.data() + s.size() || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

juce::ValueTree parametersToTree(const std::vector<std::pair<juce::String, float>>& values) {
    juce::ValueTree tree{juce::Identifier(kParametersType)};
    for (const auto& [id, value] : values) {
        juce::ValueTree param{kParamType};
        param.setProperty(kIdProp, id, nullptr);
        param.setProperty(kValueProp, floatToText(value), nullptr);
        tree.appendChild(param, nullptr);
    }
    return tree;
}

std::vector<std::pair<juce::String, float>> parametersFromTree(const juce::ValueTree& tree) {
    std::vector<std::pair<juce::String, float>> values;
    if (!tree.isValid()) return values;
    for (const auto& child : tree) {
        if (!child.hasType(kParamType)) continue;
        const juce::String id = child.getProperty(kIdProp).toString();
        float value = 0.0f;
        if (id.isEmpty() || !textToFloat(child.getProperty(kValueProp).toString(), value)) continue;
        values.emplace_back(id, value);
    }
    return values;
}

juce::ValueTree sequencerToTree(const seq::PatternBank& bank, int selectedPattern) {
    juce::ValueTree tree{juce::Identifier(kSequencerType)};
    tree.setProperty(kPatternFormatProp, kPatternFormat, nullptr);
    tree.setProperty(kPatternSizeProp, static_cast<int>(sizeof(seq::Pattern)), nullptr);
    tree.setProperty(kSongTrackSizeProp, static_cast<int>(sizeof(seq::SongTrack)), nullptr);
    tree.setProperty(kSelectedProp, selectedPattern, nullptr);
    tree.setProperty(kChainLengthProp, static_cast<int>(bank.chain.length), nullptr);
    if (!(bank.chain == seq::Chain{}))
        tree.setProperty(kChainProp, juce::Base64::toBase64(bank.chain.entries.data(), bank.chain.entries.size()),
                         nullptr);

    const seq::Pattern empty{};
    for (int i = 0; i < seq::kNumPatterns; ++i) {
        const seq::Pattern& p = bank.patterns[static_cast<std::size_t>(i)];
        if (p == empty) continue;
        juce::ValueTree child{kPatternType};
        child.setProperty(kIndexProp, i, nullptr);
        child.setProperty(kDataProp, juce::Base64::toBase64(&p, sizeof(seq::Pattern)), nullptr);
        tree.appendChild(child, nullptr);
    }

    const seq::SongTrack emptyTrack{};
    for (int i = 0; i < seq::kNumSongTracks; ++i) {
        const seq::SongTrack& t = bank.tracks[static_cast<std::size_t>(i)];
        if (t == emptyTrack) continue;
        juce::ValueTree child{kTrackType};
        child.setProperty(kIndexProp, i, nullptr);
        child.setProperty(kDataProp, juce::Base64::toBase64(&t, sizeof(seq::SongTrack)), nullptr);
        tree.appendChild(child, nullptr);
    }
    return tree;
}

bool sequencerFromTree(const juce::ValueTree& tree, seq::PatternBank& bank, int& selectedPattern) {
    if (!tree.isValid() || !tree.hasType(juce::Identifier(kSequencerType))) return false;
    // Blobs are raw Pattern bytes: only a matching format and size can be read.
    if (intProperty(tree, kPatternFormatProp, 0, 1 << 30, -1) != kPatternFormat) return false;
    if (intProperty(tree, kPatternSizeProp, 0, 1 << 30, -1) != static_cast<int>(sizeof(seq::Pattern))) return false;
    if (intProperty(tree, kSongTrackSizeProp, 0, 1 << 30, -1) != static_cast<int>(sizeof(seq::SongTrack))) return false;

    auto loaded = std::make_unique<seq::PatternBank>();
    for (const auto& child : tree) {
        if (child.hasType(kPatternType)) {
            const int index = intProperty(child, kIndexProp, 0, seq::kNumPatterns - 1, -1);
            if (index < 0) continue;
            seq::Pattern p{};
            if (decodeBlob(child.getProperty(kDataProp).toString(), &p, sizeof(seq::Pattern))) {
                sanitise(p);
                loaded->patterns[static_cast<std::size_t>(index)] = p;
            }
        } else if (child.hasType(kTrackType)) {
            const int index = intProperty(child, kIndexProp, 0, seq::kNumSongTracks - 1, -1);
            if (index < 0) continue;
            seq::SongTrack t{};
            if (decodeBlob(child.getProperty(kDataProp).toString(), &t, sizeof(seq::SongTrack))) {
                sanitise(t);
                loaded->tracks[static_cast<std::size_t>(index)] = t;
            }
        }
    }

    seq::Chain chain{};
    if (tree.hasProperty(kChainProp) &&
        decodeBlob(tree.getProperty(kChainProp).toString(), chain.entries.data(), chain.entries.size())) {
        chain.length =
            static_cast<std::uint8_t>(intProperty(tree, kChainLengthProp, 0, seq::kMaxChainLength, 0));
        sanitise(chain);
    } else {
        chain = seq::Chain{};
    }
    loaded->chain = chain;

    bank = *loaded;
    selectedPattern = intProperty(tree, kSelectedProp, 0, seq::kNumPatterns - 1, 0);
    return true;
}

juce::ValueTree userRomsToTree(const UserRoms& roms, unsigned mask) {
    juce::ValueTree tree{juce::Identifier(kUserRomsType)};
    for (int s = 0; s < kNumRomSlots; ++s) {
        const UserRom& rom = roms[static_cast<std::size_t>(s)];
        if (rom.empty() || (mask & (1U << s)) == 0U) continue;
        juce::ValueTree child{kRomType};
        child.setProperty(kSlotProp, s, nullptr);
        child.setProperty(kNameProp, rom.name, nullptr);
        child.setProperty(kDataProp, juce::Base64::toBase64(rom.image->data.data(), rom.image->data.size()), nullptr);
        tree.appendChild(child, nullptr);
    }
    return tree;
}

unsigned userRomsFromTree(const juce::ValueTree& tree, UserRoms& roms) {
    unsigned found = 0U;
    if (!tree.isValid() || !tree.hasType(juce::Identifier(kUserRomsType))) return found;
    for (const auto& child : tree) {
        if (!child.hasType(kRomType)) continue;
        const int slot = intProperty(child, kSlotProp, 0, kNumRomSlots - 1, -1);
        if (slot < 0 || (found & (1U << slot)) != 0U) continue;
        auto image = std::make_shared<RomImage>();
        if (!decodeBlob(child.getProperty(kDataProp).toString(), image->data.data(), image->data.size())) continue;
        if (!validRom(*image)) continue;
        UserRom rom;
        rom.image = std::move(image);
        rom.name = child.getProperty(kNameProp).toString().substring(0, kMaxRomNameLength);
        roms[static_cast<std::size_t>(slot)] = rom;
        found |= 1U << slot;
    }
    return found;
}

juce::ValueTree toTree(const PluginState& state) {
    juce::ValueTree root{juce::Identifier(kRootType)};
    root.setProperty(kStateVersionProp, kStateVersion, nullptr);
    root.setProperty(kProgramProp, state.program, nullptr);
    root.setProperty(kUnitSeedProp, static_cast<juce::int64>(state.model.unitSeed), nullptr);

    root.appendChild(parametersToTree(state.parameters), nullptr);
    root.appendChild(sequencerToTree(state.model.bank, state.model.selectedPattern), nullptr);
    root.appendChild(userRomsToTree(state.model.roms), nullptr);

    juce::ValueTree midi{kMidiLearnType};
    for (const auto& [controller, id] : state.midiMap) {
        juce::ValueTree map{kMapType};
        map.setProperty(kCcProp, controller, nullptr);
        map.setProperty(kParamProp, id, nullptr);
        midi.appendChild(map, nullptr);
    }
    root.appendChild(midi, nullptr);

    const UiState& ui = state.model.ui;
    juce::ValueTree uiTree{kUiType};
    uiTree.setProperty(kScaleProp, floatToText(ui.scale), nullptr);
    uiTree.setProperty(kViewProp, ui.view, nullptr);
    uiTree.setProperty(kTrackProp, ui.selectedTrack, nullptr);
    uiTree.setProperty(kStepModeProp, ui.stepEditMode, nullptr);
    uiTree.setProperty(kPageProp, ui.page, nullptr);
    uiTree.setProperty(kAuthenticProp, ui.authentic ? 1 : 0, nullptr);
    for (std::size_t i = 0; i < kPresetNameProps.size(); ++i)
        uiTree.setProperty(kPresetNameProps[i], ui.presetNames[i], nullptr);
    root.appendChild(uiTree, nullptr);

    return root;
}

juce::ValueTree migrate(juce::ValueTree root, int fromVersion) {
    // The chain of upgrades, oldest first. Each step takes the tree one version up.
    if (fromVersion < 1) {
        // v0 (pre-release builds) is the v1 layout without a version stamp.
        root.setProperty(kStateVersionProp, 1, nullptr);
    }
    // if (fromVersion < 2) root = migrateV1ToV2(root);
    return root;
}

ReadResult fromTree(const juce::ValueTree& source, const PluginState& defaults) {
    ReadResult result;
    if (!source.isValid() || !source.hasType(juce::Identifier(kRootType))) return result;

    // Missing or unreadable version stamp: the oldest layout.
    const int version = intProperty(source, kStateVersionProp, 0, 1 << 30, 0);
    if (version > kStateVersion) {
        result.status = ReadStatus::FutureVersion;
        return result;
    }
    const juce::ValueTree root = migrate(source.createCopy(), version);

    auto state = std::make_unique<PluginState>(defaults);

    // Parameters: known ids only, the rest keep their defaults. The first entry
    // for an id wins. One pass, whatever the number of entries.
    {
        std::unordered_map<juce::String, std::size_t> slots;
        slots.reserve(state->parameters.size());
        for (std::size_t i = 0; i < state->parameters.size(); ++i) slots.emplace(state->parameters[i].first, i);
        std::vector<bool> taken(state->parameters.size(), false);
        for (const auto& [loadedId, loadedValue] :
             parametersFromTree(root.getChildWithName(juce::Identifier(kParametersType)))) {
            const auto it = slots.find(loadedId);
            if (it == slots.end() || taken[it->second]) continue;
            taken[it->second] = true;
            state->parameters[it->second].second = loadedValue;
        }
    }

    sequencerFromTree(root.getChildWithName(juce::Identifier(kSequencerType)), state->model.bank,
                      state->model.selectedPattern);

    // User ROMs: exactly the slots the state holds.
    state->model.roms = UserRoms{};
    userRomsFromTree(root.getChildWithName(juce::Identifier(kUserRomsType)), state->model.roms);

    if (root.hasProperty(kUnitSeedProp)) {
        const juce::String text = root.getProperty(kUnitSeedProp).toString().trim();
        if (text.isNotEmpty() && text.containsOnly("0123456789") && text.length() <= 10) {
            const juce::int64 seed = text.getLargeIntValue();
            if (seed >= 0 && seed <= 0xFFFFFFFFLL) state->model.unitSeed = static_cast<std::uint32_t>(seed);
        }
    }
    state->program = intProperty(root, kProgramProp, 0, 1 << 20, 0);

    const juce::ValueTree midi = root.getChildWithName(kMidiLearnType);
    if (midi.isValid()) {
        state->midiMap.clear();
        for (const auto& child : midi) {
            if (!child.hasType(kMapType)) continue;
            const int controller = intProperty(child, kCcProp, 0, MidiLearn::kNumControllers - 1, -1);
            const juce::String id = child.getProperty(kParamProp).toString();
            if (controller < 0 || id.isEmpty() || id.length() > 64) continue;
            if (state->midiMap.size() >= static_cast<std::size_t>(MidiLearn::kNumControllers)) break;
            state->midiMap.emplace_back(controller, id);
        }
    }

    const juce::ValueTree uiTree = root.getChildWithName(kUiType);
    if (uiTree.isValid()) {
        UiState& ui = state->model.ui;
        float scale = ui.scale;
        if (textToFloat(uiTree.getProperty(kScaleProp).toString(), scale)) ui.scale = scale;
        ui.view = intProperty(uiTree, kViewProp, 0, kNumViews - 1, ui.view);
        ui.selectedTrack = intProperty(uiTree, kTrackProp, 0, seq::kNumTracks - 1, ui.selectedTrack);
        ui.stepEditMode = intProperty(uiTree, kStepModeProp, 0, 3, ui.stepEditMode);
        ui.page = intProperty(uiTree, kPageProp, 0, 3, ui.page);
        ui.authentic = intProperty(uiTree, kAuthenticProp, 0, 1, ui.authentic ? 1 : 0) != 0;
        for (std::size_t i = 0; i < kPresetNameProps.size(); ++i)
            if (uiTree.hasProperty(kPresetNameProps[i]))
                ui.presetNames[i] = uiTree.getProperty(kPresetNameProps[i]).toString();
        sanitise(ui);
    }

    result.status = ReadStatus::Ok;
    result.state = std::move(state);
    return result;
}

}  // namespace mw909::plugin::state
