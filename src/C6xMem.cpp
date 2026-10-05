#include "C6xMem.h"

#include <cstring>

namespace c6x {

const std::vector<Memory::Region> &Memory::regions() {
    static const std::vector<Region> r = {
        { "DSP L2 ROM", 0x00700000u, 0x00100000u, 0x11700000u },
        { "DSP L2 RAM", 0x00800000u, 0x00040000u, 0x11800000u },
        { "DSP L1P RAM", 0x00E00000u, 0x00008000u, 0x11E00000u },
        { "DSP L1D RAM", 0x00F00000u, 0x00008000u, 0x11F00000u },
        { "megamodule configuration", 0x01800000u, 0x00400000u, 0x01800000u },
        { "peripheral configuration", 0x01C00000u, 0x00400000u, 0x01C00000u },
        { "DSP L2 ROM (global)", 0x11700000u, 0x00100000u, 0x11700000u },
        { "DSP L2 RAM (global)", 0x11800000u, 0x00040000u, 0x11800000u },
        { "DSP L1P RAM (global)", 0x11E00000u, 0x00008000u, 0x11E00000u },
        { "DSP L1D RAM (global)", 0x11F00000u, 0x00008000u, 0x11F00000u },
        { "EMIFA SDRAM", 0x40000000u, 0x08000000u, 0x40000000u },
        { "EMIFA CS2-CS5", 0x60000000u, 0x08000000u, 0x60000000u },
        { "EMIFA/EMIFB control", 0x68000000u, 0x00008000u, 0x68000000u },
        { "Shared RAM", 0x80000000u, 0x00020000u, 0x80000000u },
        { "EMIFB control", 0xB0000000u, 0x00008000u, 0xB0000000u },
        { "EMIFB SDRAM", 0xC0000000u, 0x10000000u, 0xC0000000u },
    };
    return r;
}

Memory::Memory() : pages_(size_t(1) << (32 - kPageBits)) {}

bool Memory::translate(uint32_t addr, uint32_t &phys) const {
    for (const Region &r : regions())
        if (addr - r.base < r.size) { phys = r.alias + (addr - r.base); return true; }
    return false;
}
std::string Memory::regionOf(uint32_t addr) const {
    for (const Region &r : regions()) if (addr - r.base < r.size) return r.name;
    return "no region";
}

uint8_t *Memory::page(uint32_t phys, bool create) const {
    std::unique_ptr<uint8_t[]> &p = pages_[phys >> kPageBits];
    if (!p && create) {
        p.reset(new uint8_t[size_t(1) << kPageBits]);
        std::memset(p.get(), 0, size_t(1) << kPageBits);
    }
    return p.get();
}

uint8_t Memory::read8(uint32_t addr) const {
    uint32_t phys;
    if (!translate(addr, phys)) return 0;
    const uint8_t *p = page(phys, false);
    return p ? p[phys & ((1u << kPageBits) - 1)] : 0;
}
void Memory::write8(uint32_t addr, uint8_t v) {
    uint32_t phys;
    if (!translate(addr, phys)) return;
    page(phys, true)[phys & ((1u << kPageBits) - 1)] = v;
}
uint32_t Memory::read32(uint32_t addr) const {
    uint32_t phys;
    if ((addr & 3) == 0 && translate(addr, phys)) {
        const uint8_t *p = page(phys, false);
        if (!p) return 0;
        const uint8_t *b = p + (phys & ((1u << kPageBits) - 1));
        return b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
    }
    return read8(addr) | (read8(addr + 1) << 8) | (read8(addr + 2) << 16) | (uint32_t(read8(addr + 3)) << 24);
}
void Memory::write32(uint32_t addr, uint32_t v) {
    uint32_t phys;
    if ((addr & 3) == 0 && translate(addr, phys)) {
        uint8_t *b = page(phys, true) + (phys & ((1u << kPageBits) - 1));
        b[0] = uint8_t(v); b[1] = uint8_t(v >> 8); b[2] = uint8_t(v >> 16); b[3] = uint8_t(v >> 24);
        return;
    }
    for (int i = 0; i < 4; i++) write8(addr + uint32_t(i), uint8_t(v >> (8 * i)));
}

} // namespace c6x
