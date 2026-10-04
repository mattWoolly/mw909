// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "PatternModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <juce_events/juce_events.h>

namespace mw909::plugin {

namespace {

bool onMessageThread() noexcept { return juce::MessageManager::existsAndIsCurrentThread(); }

void sanitiseSnapshot(PatternModel::Snapshot& s) noexcept {
    for (auto& p : s.bank.patterns) mw909::plugin::sanitise(p);
    mw909::plugin::sanitise(s.bank.chain);
    for (auto& t : s.bank.tracks) mw909::plugin::sanitise(t);
    s.selectedPattern = std::clamp(s.selectedPattern, 0, seq::kNumPatterns - 1);
    for (auto& rom : s.roms) {
        if (rom.image != nullptr && !validRom(*rom.image)) rom.image.reset();
        if (rom.image == nullptr) rom.name = juce::String();
        if (rom.name.length() > kMaxRomNameLength) rom.name = rom.name.substring(0, kMaxRomNameLength);
    }
    mw909::plugin::sanitise(s.ui);
}

template <class T>
T clampField(T value, int low, int high) noexcept {
    const int v = static_cast<int>(value);
    return static_cast<T>(v < low ? low : (v > high ? high : v));
}

}  // namespace

bool UserRom::operator==(const UserRom& other) const noexcept {
    if (name != other.name) return false;
    if (image == other.image) return true;
    if (image == nullptr || other.image == nullptr) return false;
    return image->data == other.image->data;
}

void sanitise(seq::Pattern& p) noexcept {
    if (p.length < 1 || p.length > seq::kMaxSteps) p.length = 16;
    if (static_cast<int>(p.scale) >= static_cast<int>(seq::Scale::kNumScales)) p.scale = seq::Scale::Sixteenth;
    p.shuffle = clampField(p.shuffle, 0, seq::kMaxShuffleTicks);
    p.flamMs = clampField(p.flamMs, seq::kMinFlamMs, seq::kMaxFlamMs);
    p.flamGrace = clampField(p.flamGrace, 0, 100);
    p.subStart = clampField(p.subStart, 0, 100);
    p.subEnd = clampField(p.subEnd, 0, 100);
    p.reserved = 0;
    for (auto& len : p.trackLength)
        if (len > seq::kMaxSteps) len = 0;
    for (auto& shuffle : p.trackShuffle)
        if (shuffle > seq::kMaxShuffleTicks + 1) shuffle = 0;
}

void sanitise(seq::Chain& c) noexcept {
    if (c.length > seq::kMaxChainLength) c.length = seq::kMaxChainLength;
    for (auto& e : c.entries)
        if (e >= seq::kNumPatterns) e = 0;
}

void sanitise(seq::SongTrack& t) noexcept {
    if (t.length > seq::kMaxSongLength) t.length = seq::kMaxSongLength;
    for (auto& e : t.entries)
        if (e >= seq::kNumPatterns) e = 0;
}

void sanitise(UiState& ui) noexcept {
    if (!std::isfinite(ui.scale)) ui.scale = 1.0f;
    ui.scale = std::clamp(ui.scale, kMinUiScale, kMaxUiScale);
    ui.view = std::clamp(ui.view, 0, kNumViews - 1);
    ui.selectedTrack = std::clamp(ui.selectedTrack, 0, seq::kNumTracks - 1);
    ui.stepEditMode = std::clamp(ui.stepEditMode, 0, 3);
    ui.page = std::clamp(ui.page, 0, 3);
    for (auto& name : ui.presetNames)
        if (name.length() > 128) name = name.substring(0, 128);
}

bool validRom(const RomImage& image) noexcept {
    for (const std::uint8_t word : image.data)
        if (word > 63) return false;
    return true;
}

PatternModel::PatternModel(Machine& machine) : machine_(machine) {}

PatternModel::~PatternModel() {
    // The Machine frees whatever is still pending, active or retired in its
    // own destructor; nothing here owns a handoff.
}

const seq::Pattern& PatternModel::pattern(int index) const noexcept {
    return bank_.patterns[static_cast<std::size_t>(std::clamp(index, 0, seq::kNumPatterns - 1))];
}

const seq::SongTrack& PatternModel::songTrack(int index) const noexcept {
    return bank_.tracks[static_cast<std::size_t>(std::clamp(index, 0, seq::kNumSongTracks - 1))];
}

// --- link helpers (mutex_ held) --------------------------------------------------

void PatternModel::postBankLocked(const seq::PatternBank& bank) {
    // Free what the audio thread has finished with first, so its (bounded)
    // retire queue cannot fill up even where no service pass ever runs.
    collectRetiredLocked();

    auto handoff = std::make_unique<BankHandoff>();
    handoff->bank = bank;
    handoff->generation = ++generation_;
    // A handoff the audio thread never picked up comes straight back.
    delete machine_.postBank(handoff.release());

    // Everything queued or dirty refers to the bank this one replaces.
    dirty_.reset();
    tracksDirty_.reset();
    chainDirty_ = false;
    postedRecently_.reset();
    awaitingBank_ = true;
    adoptionObserved_ = false;
}

void PatternModel::postPatternLocked(int index) {
    const auto i = static_cast<std::size_t>(index);
    Command c;
    c.type = Command::Type::SetPattern;
    c.index = index;
    c.generation = generation_;
    c.pattern = bank_.patterns[i];
    if (machine_.post(c)) {
        dirty_.reset(i);
        postedAtTick_[i] = tick_;
        postedRecently_.set(i);
    } else {
        dirty_.set(i);
    }
}

void PatternModel::postChainLocked() {
    Command c;
    c.type = Command::Type::SetChain;
    c.generation = generation_;
    c.chain = bank_.chain;
    chainDirty_ = !machine_.post(c);
}

void PatternModel::postSongTrackLocked(int index) {
    const auto i = static_cast<std::size_t>(index);
    Command c;
    c.type = Command::Type::SetSongTrack;
    c.index = index;
    c.generation = generation_;
    c.songTrack = bank_.tracks[i];
    tracksDirty_.set(i, !machine_.post(c));
}

void PatternModel::postSelectLocked(int index, bool immediate) {
    Command c;
    c.type = Command::Type::SelectPattern;
    c.index = index;
    c.flag = immediate;
    if (machine_.post(c)) {
        selectDirty_ = false;
    } else {
        selectDirty_ = true;
        selectDirtyImmediate_ = immediate;
    }
    awaitingSelect_ = index;
}

void PatternModel::postRomLocked(int slot, const UserRom& rom) {
    collectRetiredLocked();
    auto handoff = std::make_unique<RomHandoff>();
    handoff->slot = static_cast<RomSlot>(slot);
    handoff->clear = rom.empty();
    if (!rom.empty()) handoff->image = *rom.image;
    // A handoff the audio thread never picked up comes straight back.
    delete machine_.postUserRom(handoff.release());
}

void PatternModel::collectRetiredLocked() {
    while (BankHandoff* h = machine_.takeRetiredBank()) {
        if (awaitingBank_ && h->generation == generation_) adoptionObserved_ = true;
        delete h;
    }
    while (RomHandoff* h = machine_.takeRetiredRom()) delete h;
}

unsigned PatternModel::romChanges(const UserRoms& a, const UserRoms& b) noexcept {
    unsigned changes = 0U;
    for (int s = 0; s < kNumRomSlots; ++s)
        if (!(a[static_cast<std::size_t>(s)] == b[static_cast<std::size_t>(s)])) changes |= kRomSlot << s;
    return changes;
}

unsigned PatternModel::adoptStagedLocked() {
    if (staged_ == nullptr) return 0U;
    bank_ = staged_->bank;
    selected_ = staged_->selectedPattern;
    const unsigned roms = romChanges(roms_, staged_->roms);
    roms_ = staged_->roms;
    ui_ = staged_->ui;
    staged_.reset();
    hasStaged_.store(false, std::memory_order_release);
    return kBank | kSelection | kChain | roms;
}

void PatternModel::notify(unsigned changes, const std::bitset<seq::kNumPatterns>& patterns) {
    if ((changes & kBank) != 0U) {
        listeners_.call([](Listener& l) { l.bankChanged(); });
    } else {
        for (std::size_t i = 0; i < patterns.size(); ++i)
            if (patterns.test(i)) listeners_.call([i](Listener& l) { l.patternChanged(static_cast<int>(i)); });
        if ((changes & kChain) != 0U) listeners_.call([](Listener& l) { l.chainChanged(); });
    }
    if ((changes & kSelection) != 0U) listeners_.call([](Listener& l) { l.selectionChanged(); });
    for (int s = 0; s < kNumRomSlots; ++s)
        if ((changes & (kRomSlot << s)) != 0U) listeners_.call([s](Listener& l) { l.userRomChanged(s); });
}

// --- message thread -------------------------------------------------------------

// Every mutator adopts a staged restore INSIDE the same critical section as its
// own change. Adopting first and locking again afterwards left a window in
// which a restore from another thread could post a new bank (and bump the
// generation) between the two: the edit was then made to the old model data,
// stamped with the new generation, applied by the audio thread on top of the
// restored bank, and finally overwritten in the model by the staged copy. The
// two sides stayed different for good.

seq::Pattern PatternModel::patternCopy(int index) const {
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, seq::kNumPatterns - 1));
    const std::lock_guard<std::mutex> lock(mutex_);
    return staged_ != nullptr ? staged_->bank.patterns[i] : bank_.patterns[i];
}

std::uint64_t PatternModel::beginEdit() {
    unsigned adopted = 0U;
    std::uint64_t epoch = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        adopted = adoptStagedLocked();
        epoch = restoreEpoch_;
    }
    notify(adopted, {});
    return epoch;
}

void PatternModel::setPattern(int index, const seq::Pattern& p) { commitPattern(index, p, kAnyEpoch); }
void PatternModel::setChain(const seq::Chain& c) { commitChain(c, kAnyEpoch); }
void PatternModel::setSongTrack(int index, const seq::SongTrack& t) { commitSongTrack(index, t, kAnyEpoch); }

PatternModel::Commit PatternModel::commitPattern(int index, const seq::Pattern& p, std::uint64_t epoch) {
    if (index < 0 || index >= seq::kNumPatterns) return Commit::Unchanged;

    seq::Pattern clean = p;
    sanitise(clean);
    unsigned adopted = 0U;
    Commit result = Commit::Unchanged;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        adopted = adoptStagedLocked();
        auto& slot = bank_.patterns[static_cast<std::size_t>(index)];
        if (epoch != kAnyEpoch && epoch != restoreEpoch_) {
            result = Commit::Stale;
        } else if (!(slot == clean)) {
            slot = clean;
            postPatternLocked(index);
            result = Commit::Changed;
        }
    }
    notify(adopted, {});
    if (result != Commit::Changed) return result;
    sessionDirty_.store(true, std::memory_order_release);
    listeners_.call([index](Listener& l) { l.patternChanged(index); });
    return result;
}

PatternModel::Commit PatternModel::commitChain(const seq::Chain& c, std::uint64_t epoch) {
    seq::Chain clean = c;
    sanitise(clean);
    unsigned adopted = 0U;
    Commit result = Commit::Unchanged;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        adopted = adoptStagedLocked();
        if (epoch != kAnyEpoch && epoch != restoreEpoch_) {
            result = Commit::Stale;
        } else if (!(bank_.chain == clean)) {
            bank_.chain = clean;
            postChainLocked();
            result = Commit::Changed;
        }
    }
    notify(adopted, {});
    if (result != Commit::Changed) return result;
    sessionDirty_.store(true, std::memory_order_release);
    listeners_.call([](Listener& l) { l.chainChanged(); });
    return result;
}

PatternModel::Commit PatternModel::commitSongTrack(int index, const seq::SongTrack& t, std::uint64_t epoch) {
    if (index < 0 || index >= seq::kNumSongTracks) return Commit::Unchanged;
    seq::SongTrack clean = t;
    sanitise(clean);
    unsigned adopted = 0U;
    Commit result = Commit::Unchanged;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        adopted = adoptStagedLocked();
        auto& slot = bank_.tracks[static_cast<std::size_t>(index)];
        if (epoch != kAnyEpoch && epoch != restoreEpoch_) {
            result = Commit::Stale;
        } else if (!(slot == clean)) {
            slot = clean;
            postSongTrackLocked(index);
            result = Commit::Changed;
        }
    }
    notify(adopted, {});
    if (result != Commit::Changed) return result;
    sessionDirty_.store(true, std::memory_order_release);
    listeners_.call([index](Listener& l) { l.songTrackChanged(index); });
    return result;
}

void PatternModel::replaceBank(const seq::PatternBank& bank) {
    unsigned adopted = 0U;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // `bank` may be bank_ itself (re-send the model as it stands); take
        // the copy before a staged restore replaces bank_ underneath it.
        auto incoming = std::make_unique<seq::PatternBank>(bank);
        adopted = adoptStagedLocked();
        bank_ = *incoming;
        for (auto& p : bank_.patterns) sanitise(p);
        sanitise(bank_.chain);
        for (auto& t : bank_.tracks) sanitise(t);
        postBankLocked(bank_);
    }
    // bankChanged covers whatever the adoption changed, except the selection and ROMs.
    notify(adopted & (kSelection | kRoms), {});
    sessionDirty_.store(true, std::memory_order_release);
    listeners_.call([](Listener& l) { l.bankChanged(); });
}

void PatternModel::selectPattern(int index, bool immediate) {
    if (index < 0 || index >= seq::kNumPatterns) return;

    unsigned changes = 0U;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        changes = adoptStagedLocked();
        if (selected_ != index) changes |= kSelection;
        selected_ = index;
        // Posted even when unchanged: re-selecting cancels a queued change.
        postSelectLocked(index, immediate);
    }
    notify(changes, {});
}

void PatternModel::setUnitSeed(std::uint32_t seed) {
    if (seed_.exchange(seed, std::memory_order_acq_rel) != seed) sessionDirty_.store(true, std::memory_order_release);
}

UserRom PatternModel::userRom(RomSlot slot) const {
    const auto s = static_cast<std::size_t>(std::clamp(static_cast<int>(slot), 0, kNumRomSlots - 1));
    const std::lock_guard<std::mutex> lock(mutex_);
    return staged_ != nullptr ? staged_->roms[s] : roms_[s];
}

bool PatternModel::setUserRom(RomSlot slot, const RomImage& image, const juce::String& name) {
    const int s = static_cast<int>(slot);
    if (s < 0 || s >= kNumRomSlots || !validRom(image)) return false;
    UserRom rom;
    rom.image = std::make_shared<const RomImage>(image);
    rom.name = name.substring(0, kMaxRomNameLength);
    unsigned changes = 0U;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        changes = adoptStagedLocked();
        auto& current = roms_[static_cast<std::size_t>(s)];
        if (!(current == rom)) {
            current = rom;
            postRomLocked(s, current);
            changes |= kRomSlot << s;
        }
    }
    notify(changes, {});
    if ((changes & (kRomSlot << s)) != 0U) sessionDirty_.store(true, std::memory_order_release);
    return true;
}

void PatternModel::clearUserRom(RomSlot slot) {
    const int s = static_cast<int>(slot);
    if (s < 0 || s >= kNumRomSlots) return;
    unsigned changes = 0U;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        changes = adoptStagedLocked();
        auto& current = roms_[static_cast<std::size_t>(s)];
        if (!current.empty() || current.name.isNotEmpty()) {
            current = UserRom{};
            postRomLocked(s, current);
            changes |= kRomSlot << s;
        }
    }
    notify(changes, {});
    if ((changes & (kRomSlot << s)) != 0U) sessionDirty_.store(true, std::memory_order_release);
}

UiState PatternModel::uiState() const {
    // A restore staged by another thread is already the session's UI state
    // (capture() saves it); report the same thing.
    if (hasStaged_.load(std::memory_order_acquire)) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (staged_ != nullptr) return staged_->ui;
    }
    return ui_;
}

void PatternModel::setUiState(const UiState& ui) {
    UiState clean = ui;
    sanitise(clean);
    unsigned adopted = 0U;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        adopted = adoptStagedLocked();
        ui_ = clean;
    }
    notify(adopted, {});
}

void PatternModel::service() {
    unsigned changes = 0U;
    std::bitset<seq::kNumPatterns> changed;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++tick_;
        changes |= adoptStagedLocked();

        // Order matters. Retired handoffs and the audio-side selection are
        // sampled BEFORE the notices are drained: everything the audio thread
        // pushed before those observations is then already in the queue, so
        // this drain sees (and treats as stale) every notice that predates
        // them, and the flags can be cleared safely afterwards.
        collectRetiredLocked();
        const bool adoptionObserved = adoptionObserved_;
        const PlayState& play = machine_.playState();
        const bool selectionShown =
            awaitingSelect_ >= 0 && !selectDirty_ &&
            (play.selectedPattern.load(std::memory_order_relaxed) == awaitingSelect_ ||
             play.queuedPattern.load(std::memory_order_relaxed) == awaitingSelect_);

        Notice n;
        while (machine_.poll(n)) {
            if (n.type == Notice::Type::StepRecorded) {
                if (n.pattern >= seq::kNumPatterns || n.variation >= seq::kNumVariations ||
                    n.track >= seq::kNumTracks || n.step >= seq::kMaxSteps)
                    continue;
                if (awaitingBank_) {
                    // Recorded into a bank that is being replaced (or we cannot
                    // tell): the model wins, send its version of the pattern again.
                    dirty_.set(n.pattern);
                    continue;
                }
                seq::Step& cell = bank_.patterns[n.pattern].at(n.variation, n.track, n.step);
                if (!(cell == n.cell)) {
                    cell = n.cell;
                    changed.set(n.pattern);
                }
                // An edit of this pattern was posted moments ago and may have
                // overwritten the recorded step on the audio side: re-send the
                // merged pattern so both sides hold the same data.
                if (postedRecently_.test(n.pattern)) dirty_.set(n.pattern);
            } else if (n.type == Notice::Type::PatternSelected) {
                // While one of our own selections is in flight the audio side's
                // report is stale by definition.
                if (awaitingSelect_ >= 0) continue;
                if (n.pattern < seq::kNumPatterns && static_cast<int>(n.pattern) != selected_) {
                    selected_ = n.pattern;
                    changes |= kSelection;
                }
            }
        }

        if (adoptionObserved) {
            awaitingBank_ = false;
            adoptionObserved_ = false;
        }
        if (selectionShown) awaitingSelect_ = -1;

        for (std::size_t i = 0; i < postedRecently_.size(); ++i)
            if (postedRecently_.test(i) && tick_ - postedAtTick_[i] >= 2U) postedRecently_.reset(i);

        // Re-post whatever did not fit in the command queue earlier.
        if (dirty_.any())
            for (int i = 0; i < seq::kNumPatterns; ++i)
                if (dirty_.test(static_cast<std::size_t>(i))) {
                    postPatternLocked(i);
                    if (dirty_.test(static_cast<std::size_t>(i))) break;  // still full
                }
        if (chainDirty_) postChainLocked();
        if (tracksDirty_.any())
            for (int i = 0; i < seq::kNumSongTracks; ++i)
                if (tracksDirty_.test(static_cast<std::size_t>(i))) {
                    postSongTrackLocked(i);
                    if (tracksDirty_.test(static_cast<std::size_t>(i))) break;
                }
        if (selectDirty_) postSelectLocked(selected_, selectDirtyImmediate_);
    }

    if (changed.any()) sessionDirty_.store(true, std::memory_order_release);
    notify(changes, changed);
}

// --- any non-audio thread ------------------------------------------------------------

void PatternModel::capture(Snapshot& out) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (staged_ != nullptr) {
        out = *staged_;
    } else {
        out.bank = bank_;
        out.selectedPattern = selected_;
        out.roms = roms_;
        out.ui = ui_;
    }
    out.unitSeed = seed_.load(std::memory_order_relaxed);
}

void PatternModel::applyLocked(const Snapshot& snapshot) {
    // What the audio side holds now: the staged restore's ROMs, else the model's.
    const UserRoms posted = staged_ != nullptr ? staged_->roms : roms_;
    staged_.reset();
    hasStaged_.store(false, std::memory_order_release);
    ++restoreEpoch_;
    bank_ = snapshot.bank;
    selected_ = snapshot.selectedPattern;
    roms_ = snapshot.roms;
    ui_ = snapshot.ui;
    seed_.store(snapshot.unitSeed, std::memory_order_relaxed);
    postBankLocked(bank_);
    postSelectLocked(selected_, true);
    for (int s = 0; s < kNumRomSlots; ++s)
        if (!(posted[static_cast<std::size_t>(s)] == roms_[static_cast<std::size_t>(s)]))
            postRomLocked(s, roms_[static_cast<std::size_t>(s)]);
    sessionDirty_.store(false, std::memory_order_release);
}

void PatternModel::initialise(std::unique_ptr<Snapshot> snapshot) {
    if (snapshot == nullptr) return;
    sanitiseSnapshot(*snapshot);
    const std::lock_guard<std::mutex> lock(mutex_);
    roms_ = UserRoms{};  // the audio side starts with no user ROM
    applyLocked(*snapshot);
}

void PatternModel::restore(std::unique_ptr<Snapshot> snapshot, bool keepUi) {
    if (snapshot == nullptr) return;
    sanitiseSnapshot(*snapshot);

    if (onMessageThread()) {
        unsigned changes = kBank | kSelection | kChain;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (keepUi) snapshot->ui = staged_ != nullptr ? staged_->ui : ui_;
            changes |= romChanges(staged_ != nullptr ? staged_->roms : roms_, snapshot->roms);
            applyLocked(*snapshot);
        }
        notify(changes, {});
        return;
    }

    // Foreign thread: the audio thread gets the data now, the model when the
    // message thread next looks.
    const std::lock_guard<std::mutex> lock(mutex_);
    if (keepUi) snapshot->ui = staged_ != nullptr ? staged_->ui : ui_;
    const UserRoms posted = staged_ != nullptr ? staged_->roms : roms_;
    seed_.store(snapshot->unitSeed, std::memory_order_relaxed);
    postBankLocked(snapshot->bank);
    postSelectLocked(snapshot->selectedPattern, true);
    for (int s = 0; s < kNumRomSlots; ++s)
        if (!(posted[static_cast<std::size_t>(s)] == snapshot->roms[static_cast<std::size_t>(s)]))
            postRomLocked(s, snapshot->roms[static_cast<std::size_t>(s)]);
    staged_ = std::move(snapshot);
    hasStaged_.store(true, std::memory_order_release);
    ++restoreEpoch_;
    sessionDirty_.store(false, std::memory_order_release);
}

bool PatternModel::postCommand(const Command& command) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return machine_.post(command);
}

void PatternModel::collectRetired() {
    const std::lock_guard<std::mutex> lock(mutex_);
    collectRetiredLocked();
}

int PatternModel::numDirty() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(dirty_.count()) + static_cast<int>(tracksDirty_.count()) + (chainDirty_ ? 1 : 0) +
           (selectDirty_ ? 1 : 0);
}

std::uint32_t PatternModel::generation() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

}  // namespace mw909::plugin
