#pragma once

// The C6747's address space as a program sees it (SPRS377 table 2-4, the memory map; C6747.cmd's MEMORY):
// the DSP's L2 ROM and RAM, L1P and L1D at their local addresses with their global aliases at 0x11xxxxxx,
// EMIFA's SDRAM and asynchronous chip selects, the shared RAM and EMIFB's SDRAM, and the configuration
// space of the megamodule and the peripherals, kept as plain memory. Storage is allocated a page at a time,
// as it is first written; what was never written reads as zero. An access anywhere else is a bus error.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace c6x {

class Memory {
public:
    Memory();
    // Where a byte of the region lives (the local L2/L1 addresses fold onto their global aliases), or
    // false for an address no region decodes.
    bool translate(uint32_t addr, uint32_t &phys) const;
    bool mapped(uint32_t addr) const { uint32_t p; return translate(addr, p); }
    // The region's name, for a fault: "SHRAM", "EMIFB SDRAM", ...
    std::string regionOf(uint32_t addr) const;

    // These assume the access is mapped and does not cross a region's end; the CPU checks first.
    uint8_t read8(uint32_t addr) const;
    void write8(uint32_t addr, uint8_t v);
    uint32_t read32(uint32_t addr) const;    // little-endian, at any byte address
    void write32(uint32_t addr, uint32_t v);
    uint16_t read16(uint32_t addr) const { return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8)); }
    void write16(uint32_t addr, uint16_t v) { write8(addr, static_cast<uint8_t>(v)); write8(addr + 1, static_cast<uint8_t>(v >> 8)); }
    uint64_t read64(uint32_t addr) const { return read32(addr) | (uint64_t(read32(addr + 4)) << 32); }
    void write64(uint32_t addr, uint64_t v) { write32(addr, uint32_t(v)); write32(addr + 4, uint32_t(v >> 32)); }
    void writeBytes(uint32_t addr, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) write8(addr + uint32_t(i), p[i]); }

    struct Region { const char *name; uint32_t base, size, alias; };   // alias: the global address of a local one, else base
    static const std::vector<Region> &regions();

private:
    static const unsigned kPageBits = 16;
    mutable std::vector<std::unique_ptr<uint8_t[]>> pages_;           // by physical address >> kPageBits
    uint8_t *page(uint32_t phys, bool create) const;
};

} // namespace c6x
