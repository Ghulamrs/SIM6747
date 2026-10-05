#pragma once

// The assembler: C6000 assembly text, as c90 and cpp11 write it and in the wider forms TI's own tools write, into a
// Program. Two passes - the first lays out sections and defines labels, the second resolves every symbol - over any
// number of files, each with its own local labels and a shared set of globals. An assembly error is reported as file:line: message and stops the run.

#include "Program.h"

#include <string>
#include <vector>

struct Layout {
    uint32_t memoryBytes = 32u << 20;   // the address space
    uint32_t textBase = 0x1000;         // code starts here
    std::string prelude;                // assembly text the runtime contributes (stdout ...)
};

// The names the runtime can provide; an undefined symbol among them becomes
// a native stub rather than an error.
bool assemble(const std::vector<std::string> &files, const Layout &layout,
              const std::vector<std::string> &nativeNames, Program &out,
              std::string &error);
