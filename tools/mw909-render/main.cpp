// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// mw909-render: JUCE-free command-line front end to the mw909 core. Renders
// voices and patterns to WAV, exports patterns as MIDI files, compares WAVs
// and measures realtime performance. Links mw909_core only.

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "Common.h"
#include "PatternText.h"

#ifndef MW909_VERSION
#define MW909_VERSION "unknown"
#endif
#ifndef MW909_BUILD_TYPE
#define MW909_BUILD_TYPE "unknown"
#endif

namespace {

constexpr const char* kUsage =
    R"(mw909-render - render, export and benchmark the mw909 drum machine core

USAGE
  mw909-render <command> [options]
  mw909-render --help | help [voice|pattern|midi|bench|compare|list|set|pattern-format]
  mw909-render --version

COMMANDS
  voice     render hits of one voice to a WAV file
  pattern   render measures of a pattern through the full machine (sequencer
            in host sync against a rolling transport) to a WAV file
  midi      export one variation of a pattern as a standard MIDI file (and
            check that it reads back), or import one (--read)
  bench     realtime performance report (dense pattern, typical FX, idle)
  compare   compare two WAV files (the golden-test comparer)
  list      factory memories, patterns, kits, presets and voice knobs

Options take "--name value" or "--name=value". Exit codes: 0 ok, 1 runtime
failure (I/O, bad pattern file, failed check or bench gate), 2 bad command
line. Every render is deterministic: same build, same options, same bytes.
The engine's constant latency is rendered and dropped, so the first hit or
step sounds at sample 0 (--keep-latency keeps it).
)";

constexpr const char* kVoiceHelp = R"(mw909-render voice --voice <id> --out <file.wav> [options]

  Plays hits on one voice through a complete machine (UI auditions at exact
  sample positions). Every other voice stays silent; the channel is at the
  LEVEL calibration point (0.5 = the voice's own volts), centred, on the main
  bus; the master is clean (no output stage, compressor, limiter).

  --voice <id>          bd sd lt mt ht rs cp ch oh cr rd, or 0..10
  --hits <list>         comma list, each one of
                          weak | normal | accent (instrument accent) |
                          total (total accent) | full (both) |
                          c<0..63> (an accent-DAC code) | <0..5> (volts)
                        default: normal
  --total-accent <0..1> TOTAL ACCENT knob for "total" and "full" (default 0.7)
  --spacing <s>         seconds between hits (default 0.5); hit 1 is at t = 0
  --length <s>          total length (default: hits x spacing)
  --<knob> <value>      any knob of the voice by id (see `list voices`), e.g.
                        --tune 0.8 --attack 1 --decay 0.3; the sample voices'
                        --source rom|synth|user. Also: --knob <id>=<value>
  --user-rom <file.wav> import a user ROM for the sample voice (romFromAudio);
                        selects --source user unless --source is given
  --user-region open|closed|crash|ride   ROM region (default: the voice's)
  --level <0..1>        channel LEVEL (default 0.5)
  --pan <-1..1>         channel pan (default 0)
  --master clean|stock  clean (default) or the default master chain
  --set <k>=<v>         any other setting, repeatable (help set)
  --rate <Hz>           sample rate (default 48000)
  --block <n>           processing block size, 1..8192 (default 256)
  --quality eco|standard|high     internal oversampling (default standard)
  --revision late|early hardware revision (default late)
  --seed <n>            unit seed / "serial number", decimal (leading zeros
                        allowed) or 0x hex (default 0)
  --variance <0..1>     component tolerance spread (default 0 = nominal parts)
  --stems <dir>         also write each voice's individual out, <dir>/<id>.wav
                        (dry: pre-FX, pre-master); the directory must exist
  --channels 1|2        1 writes (L+R)/2 (default 2)
  --format 16|24|32f    sample format (default 24)
  --out, -o <file>      output WAV
  --keep-latency        keep the engine latency at the start of the file
  --quiet, -q           no progress on stderr

  Example:
    mw909-render voice --voice bd --hits normal,full --spacing 1 --decay 0.8 -o bd.wav
)";

constexpr const char* kPatternHelp = R"(mw909-render pattern [source] --out <file.wav> [options]

  Source (one of):
    --memory <n|name>     factory pattern memory (default 0; see `list memories`)
      --pattern <p>       pattern in it: 0..95, I-1-1..II-3-16, or a name
    --preset <n|name>     factory preset: its memory, pattern, kit, tempo,
                          swing, variation, play mode and fill (options win)
    --text <file>         pattern text file (help pattern-format); --pattern
                          picks one of its patterns by address

  Timing:
    --measures <n>        measures to render (default 2): chain or song-track
                          entries in order, else the selected pattern
    --seconds <s>         render exactly this long instead (no tail)
    --tail <s>            extra seconds after the last measure (default 1.0)
    --tempo <bpm>         20..400 (the machine's range); default: the file's
                          tempo, else the preset's or the memory's suggested
                          tempo, else 120
    --play pattern|chain|track     PLAY MODE (default pattern); --chain = chain
    --song-track <1..8>   song track for --play track (implies it)
    --variation A|AB|B    variation switch (default A)
    --auto-fill <n>       fill every n measures: 0 (manual) 2 4 8 12 16
    --fill-slot <1..16>   fill pattern slot in the group (default 16)
    --swing-scale <0..1>  multiplier on every shuffle (default 1)
    --shuffle-ticks <0..16> / --shuffle-key <1..7>   override the pattern's shuffle
    --flam-ms <1..100> / --flam-key <1..8>           override its flam interval
    --jitter              the hardware's timing jitter; --jitter-seed <n> (implies it)

  Sound:
    --kit stock|<n>|<name>   "stock" (default) = every voice at its stock
                          settings, default mixer and master, independent of
                          the factory library; or a factory kit
    --mix kit|typical|dry  kit (default): as the kit says; typical: the tool's
                          strips + FX buses + master chain (help bench);
                          dry: no strips, no FX, clean master
    --total-accent <0..1> TOTAL ACCENT knob (default: the kit's)
    --set <k>=<v>         override one setting, repeatable (help set)
    --rate --block --quality --revision --seed --variance   as for "voice"
                          (block default 512; revision default: the kit's)

  Output:
    --out, -o <file>      main mix WAV
    --stems <dir>         also write one stem per voice, <dir>/<id>.wav, from
                          the individual outs (dry, pre-master)
    --channels, --format, --keep-latency, --quiet   as for "voice"
    --print-text          print the selected pattern in the text format to
                          stdout (no render unless --out is also given)

  The machine runs in host sync against a transport rolling from PPQ 0 at
  sample 0, so step 1 of measure 1 sounds at sample 0.

  Example:
    mw909-render pattern --memory "Classic Memory" --pattern I-1-1 --measures 4 -o beat.wav --stems stems/
)";

constexpr const char* kMidiHelp = R"(mw909-render midi [source] --out <file.mid> [options]
mw909-render midi --read <file.mid> [--variation A|B]

  Writes one measure of one variation as a format-0 standard MIDI file
  (channel 10, 480 PPQ; flams, sub-steps and shuffle as real note positions,
  weak/normal/accent velocities 48/96/127, notes from the mw909 drum map),
  then reads it back as a check: with the embedded pattern it must restore
  the pattern exactly (else exit 1); notes alone are lossy by design and the
  differing cells are reported. --read imports a file and prints it in the
  pattern text format.

  Source: --memory/--pattern, --preset or --text, as for "pattern".
  --variation A|B       which variation (default A)
  --tempo <bpm>         tempo meta event and flam timing base
  --flam-ms <1..100>    override the pattern's flam interval
  --swing-scale <0..1>  shuffle multiplier (default 1)
  --one-step-notes      notes last one step (default: a 1/16 note)
  --no-tempo            omit the tempo meta event
  --no-embed            omit the exact-pattern meta event
  --accent-note <n>     also write the accent row as notes on key n
  --print-text          print the exported pattern as text
  --out, -o <file>      output .mid
)";

constexpr const char* kBenchHelp = R"(mw909-render bench [options]

  Realtime cost of a full machine at 48 kHz, Standard quality:
    dense, no strips/FX   every voice retriggered on every step (x2..x4
                          sub-steps, flams, accents, total accent, shuffle),
                          so all 11 ring continuously; default master
    dense, typical FX     the same with the typical mix: strips (cuts, EQ,
                          compressor) on BD SD CP CH OH, FX bus 1 delay ->
                          reverb fed by sends, FX bus 2 distortion -> filter
                          carrying CP, a trigger out, and the master chain
                          (Original output stage, drive, 3-band compressor,
                          limiter, soft clip)
    silent engine         the sequencer off: what an idle instance costs
    steady, every block   every voice auditioned at the start of every
                          block: a load with no musical cadence
  For each: ns per sample, CPU % of one core, the worst block against its
  deadline, a histogram of block times and a periodicity check (block times
  folded at every period of 2..--fold-max blocks, at most a run's blocks / 8).
  The dense folds include the pattern's cadence (printed in blocks); the
  silent and steady folds have none and are gated as periodic spikes. The
  thread is pinned to the CPU it starts on. Build with the linux-release
  preset for meaningful numbers.

  --block <n>           block size (default 128)
  --seconds <s>         timed audio per run (default 4)
  --repeats <n>         runs per scenario (default 3)
  --tempo <bpm>         default 160
  --budget <pct>        dense no-FX CPU limit (default 8)
  --fx-budget <pct>     dense typical-FX CPU limit (default 12)
  --gate <pct>          worst-block limit, % of the deadline (default 25)
  --spike-gate <pct>    periodic-spike limit for the silent and steady folds:
                        fails at ratio > 1.5 with an excess over <pct> % of
                        the deadline (default 5)
  --fold-max <n>        longest fold period in blocks (default 512; raise
                        --seconds to reach it: a run needs 8 periods)
  --no-pin              do not pin the benchmark thread to one CPU
  --inject-spike <n>    check of the periodicity gate: hold every n-th block
                        to 30 % of its deadline (the gate must then fail)
  --no-gate             report only (exit 0)
  --matrix              add the rate x quality x block table
                        (--rates 44100,48000,96000 --qualities eco,standard,high
                        --blocks 32,128,1024)
  --per-voice           add the cost of each voice alone
)";

constexpr const char* kCompareHelp = R"(mw909-render compare --candidate <file.wav> --blessed <file.wav> [--tol <abs>|lsb:<n>] [--quiet]

  Compares a candidate render with a reference: format, peak and RMS of both,
  max |diff| and where it is, RMS of the difference, the first differing
  sample and the difference per tenth of the file. Passes when formats match
  and max |diff| <= tol (default one LSB of the reference's bit depth, 0 for
  float; the golden tests pass --tol 0). A NaN or infinite sample on either
  side always fails. Exit 0 match, 1 mismatch, 2 unreadable file or usage.
  (Also built as the standalone mw909-wavcompare, which the golden tests
  use.)
)";

constexpr const char* kListHelp = R"(mw909-render list [memories | patterns [memory] | kits | presets | voices]

  Prints the factory content the other commands accept by index or name, and
  every voice's knobs with their ranges and defaults. No argument: all but
  the pattern lists.
)";

void printHelp(const std::string& topic)
{
    using mw909::tools::kPatternTextHelp;
    using mw909::tools::kSetHelp;
    if (topic == "voice")
        std::fputs(kVoiceHelp, stdout);
    else if (topic == "pattern")
        std::fputs(kPatternHelp, stdout);
    else if (topic == "midi")
        std::fputs(kMidiHelp, stdout);
    else if (topic == "bench")
        std::fputs(kBenchHelp, stdout);
    else if (topic == "compare")
        std::fputs(kCompareHelp, stdout);
    else if (topic == "list")
        std::fputs(kListHelp, stdout);
    else if (topic == "set")
        std::fputs(kSetHelp, stdout);
    else if (topic == "pattern-format")
        std::fputs(kPatternTextHelp, stdout);
    else
    {
        for (const char* t : {kUsage, kVoiceHelp, kPatternHelp, kMidiHelp, kBenchHelp, kCompareHelp, kListHelp})
        {
            std::fputs(t, stdout);
            std::fputs("\n", stdout);
        }
        std::fputs("mw909-render help set\n\n", stdout);
        std::fputs(kSetHelp, stdout);
        std::fputs("\n", stdout);
        std::fputs(kPatternTextHelp, stdout);
    }
}

bool wantsHelp(const std::vector<std::string>& args)
{
    for (const auto& a : args)
        if (a == "--help" || a == "-h")
            return true;
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace mw909::tools;
    if (argc < 2)
    {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (command == "--help" || command == "-h" || command == "help")
    {
        printHelp(args.empty() ? std::string() : args[0]);
        return 0;
    }
    if (command == "--version")
    {
        std::printf("mw909-render %s (%s build, g++ %s)\n", MW909_VERSION, MW909_BUILD_TYPE, __VERSION__);
        return 0;
    }
    if (wantsHelp(args))
    {
        printHelp(command);
        return 0;
    }

    const bool known = command == "voice" || command == "pattern" || command == "midi" || command == "bench"
                       || command == "compare" || command == "list";
    try
    {
        if (command == "voice")
            return cmdVoice(args);
        if (command == "pattern")
            return cmdPattern(args);
        if (command == "midi")
            return cmdMidi(args);
        if (command == "bench")
            return cmdBench(args);
        if (command == "compare")
            return cmdCompare(args);
        if (command == "list")
            return cmdList(args);
        throw UsageError("unknown command '" + command + "'");
    }
    catch (const UsageError& e)
    {
        std::fprintf(stderr, "mw909-render: %s\n(see: mw909-render help %s)\n", e.what(),
                     known ? command.c_str() : "");
        return 2;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "mw909-render: error: %s\n", e.what());
        return 1;
    }
}
