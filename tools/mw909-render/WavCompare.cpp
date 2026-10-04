// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// mw909-wavcompare: compares a candidate render against a blessed reference
// and prints diagnostics a reviewer can act on without opening a DAW: format,
// peak/RMS of both, max |diff| and where it is, RMS of the difference in dBFS
// and relative to the reference, the first differing sample, and the
// difference per tenth of the file.
//
//   mw909-wavcompare --candidate new.wav --blessed ref.wav [--tol <x>|lsb:<n>]
//
// Pass when the formats match and max |diff| <= tol. Default tol is one LSB of
// the blessed file's bit depth (0 for float files): the reference platform is
// this machine and the core is deterministic, so a blessed render reproduces
// bit for bit; one LSB absorbs a single rounding flip at the quantiser.
// Exit codes: 0 pass, 1 mismatch, 2 usage or unreadable file.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Wav.h"

namespace {

void usage()
{
    std::fputs(
        "usage: mw909-wavcompare --candidate <file.wav> --blessed <file.wav> [--tol <abs>|lsb:<n>] "
        "[--quiet]\n"
        "       exit 0 = match within tolerance, 1 = mismatch, 2 = usage / unreadable file\n",
        stderr);
}

} // namespace

int main(int argc, char** argv)
{
    using namespace mw909::tools;
    std::string candidatePath, blessedPath, tolText;
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--candidate")
            candidatePath = next();
        else if (a == "--blessed")
            blessedPath = next();
        else if (a == "--tol")
            tolText = next();
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--help" || a == "-h")
        {
            usage();
            return 0;
        }
        else
        {
            usage();
            return 2;
        }
    }
    if (candidatePath.empty() || blessedPath.empty())
    {
        usage();
        return 2;
    }

    return compareWavFiles(candidatePath, blessedPath, tolText, quiet);
}
