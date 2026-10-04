// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "PresetManager.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "mw909/Factory.h"
#include "mw909Processor.h"
#include "state/StateCodec.h"

#ifndef MW909_PRODUCT_NAME
 #define MW909_PRODUCT_NAME "mw909"
#endif

namespace mw909::plugin {

namespace {

const juce::Identifier kPresetType{"mw909preset"};
const juce::Identifier kFormatVersionProp{"formatVersion"};
const juce::Identifier kKindProp{"kind"};
const juce::Identifier kNameProp{"name"};
const juce::Identifier kCategoryProp{"category"};

constexpr int kPresetFormatVersion = 1;
constexpr auto kMaxPresetBytes = static_cast<juce::int64>(state::kMaxStateBytes);

constexpr std::array<const char*, kNumPresetKinds> kKindNames{"kit", "patterns", "full"};
constexpr std::array<const char*, kNumPresetKinds> kKindFolders{"Kits", "Patterns", "Full"};

std::size_t slot(PresetKind kind) { return static_cast<std::size_t>(kind); }

juce::String toString(std::string_view text) { return juce::String(std::string(text)); }

bool kindFromString(const juce::String& text, PresetKind& kind) {
    for (int i = 0; i < kNumPresetKinds; ++i)
        if (text == kKindNames[static_cast<std::size_t>(i)]) {
            kind = static_cast<PresetKind>(i);
            return true;
        }
    return false;
}

juce::ValueTree readPresetFile(const juce::File& file) {
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > kMaxPresetBytes) return {};
    // Read bytes, not a String: anything in the preset folder may be opened,
    // and String::fromUTF8 (which loadFileAsString ends in) asserts on bytes
    // that are not UTF-8. Our own files are plain UTF-8 XML; a byte-order mark
    // is tolerated, anything else that is not valid UTF-8 is refused.
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() == 0) return {};
    const auto* text = static_cast<const char*>(bytes.getData());
    std::size_t size = bytes.getSize();
    if (size >= 3 && std::memcmp(text, "\xEF\xBB\xBF", 3) == 0) {
        text += 3;
        size -= 3;
    }
    if (size == 0 || !state::isValidUtf8(text, size)) return {};
    if (state::hasDoctype(text, size) || state::xmlNestingExceeds(text, size, state::kMaxXmlDepth)) return {};
    const auto xml = juce::parseXML(juce::String(juce::CharPointer_UTF8(text), juce::CharPointer_UTF8(text + size)));
    if (xml == nullptr) return {};
    juce::ValueTree tree = juce::ValueTree::fromXml(*xml);
    return tree.hasType(kPresetType) ? tree : juce::ValueTree{};
}

}  // namespace

PresetManager::PresetManager(::mw909Processor& processor) : processor_(processor), root_(defaultUserRoot()) {}

juce::File PresetManager::defaultUserRoot() {
    const juce::String overridden = juce::SystemStats::getEnvironmentVariable("MW909_PRESET_DIR", {});
    if (overridden.isNotEmpty() && juce::File::isAbsolutePath(overridden)) return juce::File(overridden);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("mwAudio")
        .getChildFile(MW909_PRODUCT_NAME)
        .getChildFile("Presets");
}

void PresetManager::setUserRoot(const juce::File& root) {
    root_ = root;
    scanned_ = false;
    for (auto& list : user_) list.clear();
    for (int k = 0; k < kNumPresetKinds; ++k)
        listeners_.call([k](Listener& l) { l.presetListChanged(static_cast<PresetKind>(k)); });
}

juce::File PresetManager::userDirectory(PresetKind kind) const {
    return root_.getChildFile(kKindFolders[slot(kind)]);
}

void PresetManager::refresh() {
    for (int k = 0; k < kNumPresetKinds; ++k) {
        const auto kind = static_cast<PresetKind>(k);
        const juce::File dir = userDirectory(kind);
        dir.createDirectory();

        auto& list = user_[slot(kind)];
        list.clear();
        for (const auto& file :
             dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kFileExtension)) {
            PresetEntry entry;
            entry.name = file.getFileNameWithoutExtension();
            entry.category = "User";
            entry.file = file;
            list.push_back(entry);
        }
        std::sort(list.begin(), list.end(), [](const PresetEntry& a, const PresetEntry& b) {
            return a.name.compareNatural(b.name) < 0;
        });
    }
    scanned_ = true;
    for (int k = 0; k < kNumPresetKinds; ++k)
        listeners_.call([k](Listener& l) { l.presetListChanged(static_cast<PresetKind>(k)); });
}

void PresetManager::ensureScanned() {
    if (!scanned_) refresh();
}

int PresetManager::numFactoryPresets(PresetKind kind) const {
    switch (kind) {
        case PresetKind::Kit:      return std::max(0, factory::numKits());
        case PresetKind::Patterns: return std::max(0, factory::numBanks());
        case PresetKind::Full:     return std::max(0, factory::numPresets());
    }
    return 0;
}

int PresetManager::numPresets(PresetKind kind) {
    ensureScanned();
    return numFactoryPresets(kind) + static_cast<int>(user_[slot(kind)].size());
}

PresetEntry PresetManager::preset(PresetKind kind, int index) {
    ensureScanned();
    PresetEntry entry;
    const int numFactory = numFactoryPresets(kind);
    if (index < 0) return entry;
    if (index < numFactory) {
        entry.factory = true;
        entry.factoryIndex = index;
        switch (kind) {
            case PresetKind::Kit:
                entry.name = toString(factory::kit(index).name);
                entry.category = toString(factory::kit(index).category);
                break;
            case PresetKind::Patterns:
                entry.name = toString(factory::bankInfo(index).name);
                entry.category = toString(factory::bankInfo(index).category);
                break;
            case PresetKind::Full:
                entry.name = toString(factory::preset(index).name);
                entry.category = toString(factory::preset(index).category);
                break;
        }
        return entry;
    }
    const auto& list = user_[slot(kind)];
    const auto userIndex = static_cast<std::size_t>(index - numFactory);
    return userIndex < list.size() ? list[userIndex] : entry;
}

juce::StringArray PresetManager::names(PresetKind kind) {
    juce::StringArray result;
    const int n = numPresets(kind);
    for (int i = 0; i < n; ++i) result.add(preset(kind, i).name);
    return result;
}

juce::String PresetManager::currentName(PresetKind kind) const {
    return processor_.patterns().uiState().presetNames[slot(kind)];
}

void PresetManager::setCurrentName(PresetKind kind, const juce::String& name) {
    UiState ui = processor_.patterns().uiState();
    ui.presetNames[slot(kind)] = name;
    processor_.patterns().setUiState(ui);
}

int PresetManager::currentIndex(PresetKind kind) {
    const juce::String name = currentName(kind);
    if (name.isEmpty()) return -1;
    const int n = numPresets(kind);
    for (int i = 0; i < n; ++i)
        if (preset(kind, i).name == name) return i;
    return -1;
}

bool PresetManager::step(PresetKind kind, int delta) {
    const int n = numPresets(kind);
    if (n <= 0) return false;
    const int current = currentIndex(kind);
    const int next = current < 0 ? (delta > 0 ? 0 : n - 1) : ((current + delta) % n + n) % n;
    return load(kind, next);
}

bool PresetManager::loadFactory(PresetKind kind, int index) {
    if (index < 0 || index >= numFactoryPresets(kind)) return false;

    const auto loadKit = [this](int kitIndex) {
        std::vector<std::pair<juce::String, float>> values;
        for (const auto& [id, value] : Parameters::kitValues(factory::kit(kitIndex)))
            values.emplace_back(juce::String(id), value);
        processor_.setParameterValues(values, true);
        setCurrentName(PresetKind::Kit, toString(factory::kit(kitIndex).name));
    };
    const auto loadBank = [this](int bankIndex) {
        auto bank = std::make_unique<seq::PatternBank>();
        factory::buildBank(bankIndex, *bank);
        processor_.patterns().replaceBank(*bank);
        setCurrentName(PresetKind::Patterns, toString(factory::bankInfo(bankIndex).name));
    };

    switch (kind) {
        case PresetKind::Kit:
            loadKit(index);
            break;
        case PresetKind::Patterns:
            // A memory on its own brings its suggested tempo.
            loadBank(index);
            processor_.setParameterValues({{juce::String(paramid::tempo), factory::bankInfo(index).tempo}}, true);
            break;
        case PresetKind::Full: {
            // A factory preset pairs a kit with a memory, a starting pattern
            // and the sequencer feel (tempo, shuffle depth, variation, play
            // mode, song track, auto fill). Transport and MIDI settings stay.
            const factory::Preset& p = factory::preset(index);
            loadKit(std::clamp(p.kit, 0, std::max(0, factory::numKits() - 1)));
            loadBank(std::clamp(p.bank, 0, std::max(0, factory::numBanks() - 1)));
            processor_.patterns().selectPattern(std::clamp(p.pattern, 0, seq::kNumPatterns - 1), true);
            std::vector<std::pair<juce::String, float>> values;
            for (const auto& [id, value] : Parameters::presetValues(p)) values.emplace_back(juce::String(id), value);
            processor_.setParameterValues(values, true);
            setCurrentName(PresetKind::Full, toString(p.name));
            processor_.factoryProgramLoaded(index);
            break;
        }
    }
    return true;
}

bool PresetManager::applyTree(const juce::ValueTree& tree, const juce::String& name) {
    PresetKind kind{};
    if (!tree.hasType(kPresetType) || !kindFromString(tree.getProperty(kKindProp).toString(), kind)) return false;

    switch (kind) {
        case PresetKind::Kit: {
            const auto loaded = state::parametersFromTree(tree.getChildWithName(juce::Identifier(state::kParametersType)));
            // Only the kit's own parameters, whatever else the file lists.
            std::vector<std::pair<juce::String, float>> values;
            for (const auto& id : kitParameterIds())
                for (const auto& [loadedId, value] : loaded)
                    if (loadedId == juce::String(id)) {
                        values.emplace_back(loadedId, value);
                        break;
                    }
            if (values.empty()) return false;
            processor_.setParameterValues(values, true);
            // The user ROMs the kit's voices play, when it carries them.
            UserRoms roms{};
            const unsigned found =
                state::userRomsFromTree(tree.getChildWithName(juce::Identifier(state::kUserRomsType)), roms);
            for (int s = 0; s < kNumRomSlots; ++s)
                if ((found & (1U << s)) != 0U) {
                    const UserRom& rom = roms[static_cast<std::size_t>(s)];
                    processor_.patterns().setUserRom(static_cast<RomSlot>(s), *rom.image, rom.name);
                }
            break;
        }
        case PresetKind::Patterns: {
            auto bank = std::make_unique<seq::PatternBank>();
            int selected = 0;
            if (!state::sequencerFromTree(tree.getChildWithName(juce::Identifier(state::kSequencerType)), *bank,
                                          selected))
                return false;
            processor_.patterns().replaceBank(*bank);
            break;
        }
        case PresetKind::Full: {
            const auto defaults = processor_.makeDefaultState();
            auto result = state::fromTree(tree.getChildWithName(juce::Identifier(state::kRootType)), *defaults);
            if (result.status != state::ReadStatus::Ok || result.state == nullptr) return false;
            const auto presetNames = result.state->model.ui.presetNames;
            processor_.applyState(*result.state, false);
            setCurrentName(PresetKind::Kit, presetNames[slot(PresetKind::Kit)]);
            setCurrentName(PresetKind::Patterns, presetNames[slot(PresetKind::Patterns)]);
            break;
        }
    }
    setCurrentName(kind, name);
    listeners_.call([kind](Listener& l) { l.presetLoaded(kind); });
    return true;
}

bool PresetManager::load(PresetKind kind, int index) {
    if (index < 0) return false;
    // Factory presets never touch the disk (host program changes land here).
    if (index < numFactoryPresets(kind)) {
        if (!loadFactory(kind, index)) return false;
        listeners_.call([kind](Listener& l) { l.presetLoaded(kind); });
        return true;
    }
    ensureScanned();
    if (index >= numPresets(kind)) return false;
    return loadFile(preset(kind, index).file);
}

bool PresetManager::loadFile(const juce::File& file) {
    const juce::ValueTree tree = readPresetFile(file);
    if (!tree.isValid()) return false;
    return applyTree(tree, file.getFileNameWithoutExtension());
}

bool PresetManager::saveUser(PresetKind kind, const juce::String& name) {
    const juce::String legal = juce::File::createLegalFileName(name.trim()).trim();
    if (legal.isEmpty() || legal.length() > 100) return false;

    juce::ValueTree tree{kPresetType};
    tree.setProperty(kFormatVersionProp, kPresetFormatVersion, nullptr);
    tree.setProperty(kKindProp, kKindNames[slot(kind)], nullptr);
    tree.setProperty(kNameProp, legal, nullptr);
    tree.setProperty(kCategoryProp, "User", nullptr);

    // The name goes into the session first, so a full preset carries its own name.
    setCurrentName(kind, legal);
    const auto captured = processor_.captureState();
    switch (kind) {
        case PresetKind::Kit: {
            std::vector<std::pair<juce::String, float>> values;
            for (const auto& id : kitParameterIds())
                for (const auto& [sid, value] : captured->parameters)
                    if (sid == juce::String(id)) {
                        values.emplace_back(sid, value);
                        break;
                    }
            tree.appendChild(state::parametersToTree(values), nullptr);
            // A voice set to Source = User needs its image: the kit carries
            // the ROM slots its voices play from.
            unsigned mask = 0U;
            const auto usesUser = [&captured](int voice) {
                const juce::String id(paramid::knob(voice, knob::PcmSource));
                for (const auto& [sid, value] : captured->parameters)
                    if (sid == id) return static_cast<int>(value + 0.5f) == static_cast<int>(PcmSource::User);
                return false;
            };
            if (usesUser(CH) || usesUser(OH)) mask |= 1U << static_cast<int>(RomSlot::Hat);
            if (usesUser(CR)) mask |= 1U << static_cast<int>(RomSlot::Crash);
            if (usesUser(RD)) mask |= 1U << static_cast<int>(RomSlot::Ride);
            if (mask != 0U) tree.appendChild(state::userRomsToTree(captured->model.roms, mask), nullptr);
            break;
        }
        case PresetKind::Patterns:
            tree.appendChild(state::sequencerToTree(captured->model.bank, captured->model.selectedPattern), nullptr);
            break;
        case PresetKind::Full:
            tree.appendChild(state::toTree(*captured), nullptr);
            break;
    }

    const juce::File dir = userDirectory(kind);
    if (!dir.createDirectory().wasOk()) return false;
    const auto xml = tree.createXml();
    if (xml == nullptr || !xml->writeTo(dir.getChildFile(legal + kFileExtension))) return false;

    refresh();
    return true;
}

bool PresetManager::deleteUser(PresetKind kind, int index) {
    ensureScanned();
    const PresetEntry entry = preset(kind, index);
    if (entry.factory || entry.file == juce::File()) return false;
    // Only ever a preset file inside this kind's own directory.
    if (!entry.file.existsAsFile() || !entry.file.hasFileExtension(kFileExtension) ||
        entry.file.getParentDirectory() != userDirectory(kind))
        return false;
    if (!entry.file.deleteFile()) return false;
    refresh();
    return true;
}

}  // namespace mw909::plugin
