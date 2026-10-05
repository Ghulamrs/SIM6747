#pragma once

// A C6000 program image as TI's tools leave it: an ELF (EABI) or COFF (COFF ABI) .out or .obj from
// cl6x/lnk6x, a hex6x image (TI-TXT, Intel, Motorola-S), or a raw binary at a given address. What is kept
// is what a loader needs - the bytes at their addresses, which ranges hold code, the entry point and the
// symbols - and nothing of the file format beyond that.

#include "C6xDecode.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace c6x {

struct Section {
    std::string name;
    uint32_t addr = 0;
    std::vector<uint8_t> bytes;    // empty for an uninitialised one (.bss, .stack, .sysmem)
    uint32_t size = 0;             // in bytes, initialised or not
    bool code = false;
};

class Image : public CodeReader {
public:
    std::vector<Section> sections;
    std::map<uint32_t, std::string> symbolsAt;     // address -> name, for listings and traces
    std::map<uint32_t, int> rankAt;                // how much that name is wanted (C6xImage.cpp, nameRank)
    std::map<std::string, uint32_t> symbols;       // name -> address
    uint32_t entry = 0;
    bool hasEntry = false;
    std::string format;                            // "elf", "coff", "ti-txt", "intel", "srec", "bin"

    bool read32(uint32_t addr, uint32_t &w) const override;
    bool read8(uint32_t addr, uint8_t &b) const;
    // The symbol that names addr, or the nearest one before it, with the distance: "main+12".
    std::string where(uint32_t addr) const;
};

// Load path into img. The format is told by the file's first bytes; a raw binary is taken only when
// binBase is given (it has no header to tell it by). False, with why, if the file cannot be read.
bool loadImage(const std::string &path, Image &img, std::string &why, bool rawBinary = false, uint32_t binBase = 0);

} // namespace c6x
