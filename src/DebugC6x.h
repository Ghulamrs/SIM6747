#pragma once

// --run --debug: the machine-code CPU under Debug.h's shell. The source positions come from the
// image's DWARF .debug_line (cl6x -g writes one; what ASM6x and LNK6x leave out is said by name),
// the functions from its STT_FUNC symbols, and the end and the status from the host's C$$EXIT.

#include "C6xCpu.h"
#include "C6xImage.h"

#include <functional>
#include <string>

namespace c6x {

struct DebugEnds {
    std::function<bool()> ended;    // the program reached C$$EXIT, or the CPU stopped
    std::function<int()> status;    // its exit status, as --run reports it
};

// Run the session over cpu and img; the exit status to return.
int debugRun(Cpu6x &cpu, const Image &img, const std::string &path, const DebugEnds &ends);

} // namespace c6x
