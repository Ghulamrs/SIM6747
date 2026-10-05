#include "C6xImage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace c6x {

bool Image::read8(uint32_t addr, uint8_t &b) const {
    for (const Section &s : sections) {
        if (addr < s.addr || addr - s.addr >= s.bytes.size()) continue;
        b = s.bytes[addr - s.addr];
        return true;
    }
    return false;
}
bool Image::read32(uint32_t addr, uint32_t &w) const {
    for (const Section &s : sections) {
        if (addr < s.addr || addr - s.addr + 4 > s.bytes.size()) continue;
        const uint8_t *p = &s.bytes[addr - s.addr];
        w = p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
        return true;
    }
    return false;
}
std::string Image::where(uint32_t addr) const {
    std::map<uint32_t, std::string>::const_iterator i = symbolsAt.upper_bound(addr);
    if (i == symbolsAt.begin()) return "";
    --i;
    if (i->first == addr) return i->second;
    return i->second + "+" + std::to_string(addr - i->first);
}

static uint16_t u16(const std::vector<uint8_t> &f, size_t o) { return static_cast<uint16_t>(f[o] | (f[o + 1] << 8)); }
static uint32_t u32(const std::vector<uint8_t> &f, size_t o) { return f[o] | (f[o + 1] << 8) | (f[o + 2] << 16) | (uint32_t(f[o + 3]) << 24); }

// How much a listing or a fault wants a name for its address: a function's over anything else, then a C name, then
// the linker's and runtime's own (__TI_exidx_linkto_scn_start_N alias every function start), assembler
// temporaries ($C$L1, .L1) last.
static int nameRank(const std::string &n, bool function) {
    if (n[0] == '$' || n.compare(0, 2, ".L") == 0) return 4;
    if (n.compare(0, 27, "__TI_exidx_linkto_scn_start") == 0 || n.compare(0, 14, "__c6xabi_extab") == 0) return 3;
    if (n.compare(0, 4, "__TI") == 0 || n.find('$') != std::string::npos) return 2;
    return function ? 0 : 1;
}
static void addSymbol(Image &img, const std::string &name, uint32_t value, bool function = false) {
    if (name.empty()) return;
    img.symbols[name] = value;
    std::map<uint32_t, std::string>::iterator i = img.symbolsAt.find(value);
    int rank = nameRank(name, function);
    if (i == img.symbolsAt.end()) { img.symbolsAt[value] = name; img.rankAt[value] = rank; }
    else if (rank < img.rankAt[value]) { i->second = name; img.rankAt[value] = rank; }
}

// ---- ELF (the C6000 EABI, SPRAB89) ----------------------------------------------------------------------
static bool loadElf(const std::vector<uint8_t> &f, Image &img, std::string &why) {
    if (f.size() < 52 || f[4] != 1) { why = "not a 32-bit ELF file"; return false; }
    if (f[5] != 1) { why = "a big-endian ELF file: only the little-endian C6747 is modelled"; return false; }
    if (u16(f, 18) != 140) { why = "an ELF file for machine " + std::to_string(u16(f, 18)) + ", not the TI C6000 (140)"; return false; }
    img.format = "elf";
    uint16_t type = u16(f, 16);
    img.entry = u32(f, 24);
    img.hasEntry = type == 2;
    uint32_t shoff = u32(f, 32);
    uint16_t shentsize = u16(f, 46), shnum = u16(f, 48), shstrndx = u16(f, 50);
    if (shoff == 0 || shoff + uint64_t(shnum) * shentsize > f.size()) { why = "the ELF section headers are outside the file"; return false; }
    struct Sh { uint32_t name, type, flags, addr, off, size, link; };
    std::vector<Sh> sh(shnum);
    for (unsigned i = 0; i < shnum; i++) {
        size_t o = shoff + size_t(i) * shentsize;
        sh[i] = Sh{ u32(f, o), u32(f, o + 4), u32(f, o + 8), u32(f, o + 12), u32(f, o + 16), u32(f, o + 20), u32(f, o + 24) };
    }
    auto str = [&](unsigned tab, uint32_t at) -> std::string {
        if (tab >= sh.size()) return "";
        size_t o = sh[tab].off + at;
        std::string s;
        while (o < f.size() && f[o]) s += static_cast<char>(f[o++]);
        return s;
    };
    for (unsigned i = 0; i < shnum; i++) {
        const Sh &s = sh[i];
        const uint32_t SHF_ALLOC = 2, SHF_EXEC = 4, SHT_NOBITS = 8, SHT_PROGBITS = 1;
        if (!(s.flags & SHF_ALLOC) || s.size == 0) continue;
        Section sec;
        sec.name = str(shstrndx, s.name);
        sec.addr = s.addr;
        sec.size = s.size;
        sec.code = (s.flags & SHF_EXEC) != 0;
        if (s.type != SHT_NOBITS) {
            if (s.type != SHT_PROGBITS && s.type < 0x70000000u && s.type != 14 && s.type != 15) continue;  // init/fini arrays are data
            if (uint64_t(s.off) + s.size > f.size()) { why = "section " + sec.name + " runs past the end of the file"; return false; }
            sec.bytes.assign(f.begin() + s.off, f.begin() + s.off + s.size);
        }
        img.sections.push_back(sec);
    }
    for (unsigned i = 0; i < shnum; i++) {
        if (sh[i].type != 2) continue;                       // SHT_SYMTAB
        for (uint32_t o = sh[i].off; o + 16 <= sh[i].off + sh[i].size && o + 16 <= f.size(); o += 16) {
            uint32_t name = u32(f, o), value = u32(f, o + 4);
            uint8_t info = f[o + 12];
            uint16_t shndx = u16(f, o + 14);
            unsigned stype = info & 0xf;
            if (shndx == 0 || stype == 3 || stype == 4) continue;   // undefined, section, file
            addSymbol(img, str(sh[i].link, name), value, stype == 2);
        }
    }
    return true;
}

// ---- COFF (TI's COFF version 2, SPRAAO8) ------------------------------------------------------------------
static bool loadCoff(const std::vector<uint8_t> &f, Image &img, std::string &why) {
    if (f.size() < 22) { why = "a COFF file too short for its header"; return false; }
    uint16_t nsec = u16(f, 2);
    uint32_t symptr = u32(f, 8), nsyms = u32(f, 12);
    uint16_t opthdr = u16(f, 16), target = u16(f, 20);
    if (target != 0x0099) { why = "a COFF file for target 0x" + std::to_string(target) + ", not the C6000 (0x99)"; return false; }
    img.format = "coff";
    if (opthdr >= 28) { img.entry = u32(f, 22 + 16); img.hasEntry = true; }
    size_t sh = 22 + opthdr;
    uint32_t strtab = symptr + nsyms * 18;
    auto name8 = [&](size_t o) -> std::string {
        if (u32(f, o) == 0) {                                  // a long name: an offset into the string table
            std::string s; size_t p = strtab + u32(f, o + 4);
            while (p < f.size() && f[p]) s += static_cast<char>(f[p++]);
            return s;
        }
        std::string s;
        for (int i = 0; i < 8 && f[o + i]; i++) s += static_cast<char>(f[o + i]);
        return s;
    };
    for (unsigned i = 0; i < nsec; i++, sh += 48) {
        if (sh + 48 > f.size()) { why = "the COFF section headers run past the end of the file"; return false; }
        uint32_t paddr = u32(f, sh + 8), size = u32(f, sh + 16), data = u32(f, sh + 20), flags = u32(f, sh + 40);
        const uint32_t DSECT = 1, NOLOAD = 2, COPY = 0x10, TEXT = 0x20, BSS = 0x80;
        if (size == 0 || (flags & (DSECT | NOLOAD | COPY))) continue;
        Section sec;
        sec.name = name8(sh);
        sec.addr = paddr;
        sec.size = size;
        sec.code = (flags & TEXT) != 0;
        if (!(flags & BSS) && data != 0) {
            if (uint64_t(data) + size > f.size()) { why = "section " + sec.name + " runs past the end of the file"; return false; }
            sec.bytes.assign(f.begin() + data, f.begin() + data + size);
        }
        img.sections.push_back(sec);
    }
    for (uint32_t s = 0; s < nsyms && symptr + (s + 1) * 18 <= f.size(); s++) {
        size_t o = symptr + s * 18;
        int16_t scnum = static_cast<int16_t>(u16(f, o + 12));
        uint8_t sclass = f[o + 16], naux = f[o + 17];
        // COFF type 0x20 is DT_FCN: a function
        if (scnum > 0 && (sclass == 2 || sclass == 3 || sclass == 6)) addSymbol(img, name8(o), u32(f, o + 8), (u16(f, o + 14) & 0x30) == 0x20);
        s += naux;
    }
    return true;
}

// ---- hex6x images ------------------------------------------------------------------------------------------
// Bytes at addresses, gathered into runs: each contiguous run becomes a section.
static void addBytes(std::map<uint32_t, uint8_t> &m, uint32_t a, const std::vector<uint8_t> &b) {
    for (size_t i = 0; i < b.size(); i++) m[a + static_cast<uint32_t>(i)] = b[i];
}
static void runsToSections(const std::map<uint32_t, uint8_t> &m, Image &img) {
    Section cur;
    bool open = false;
    uint32_t next = 0;
    for (std::map<uint32_t, uint8_t>::const_iterator i = m.begin(); i != m.end(); ++i) {
        if (!open || i->first != next) {
            if (open) { cur.size = static_cast<uint32_t>(cur.bytes.size()); img.sections.push_back(cur); }
            cur = Section(); cur.addr = i->first; cur.code = true; open = true;
            char b[16]; std::snprintf(b, sizeof b, "@%08x", i->first); cur.name = b;
        }
        cur.bytes.push_back(i->second);
        next = i->first + 1;
    }
    if (open) { cur.size = static_cast<uint32_t>(cur.bytes.size()); img.sections.push_back(cur); }
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static bool hexBytes(const std::string &s, size_t from, size_t n, std::vector<uint8_t> &out) {
    out.clear();
    if (from + 2 * n > s.size()) return false;
    for (size_t i = 0; i < n; i++) {
        int h = hexval(s[from + 2 * i]), l = hexval(s[from + 2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out.push_back(static_cast<uint8_t>(h << 4 | l));
    }
    return true;
}

// TI-TXT (hex6x --ti_txt): "@ADDR" lines, then lines of hex bytes separated by spaces, ending with "q".
static bool loadTiTxt(const std::string &text, Image &img, std::string &why) {
    std::map<uint32_t, uint8_t> m;
    std::istringstream in(text);
    std::string line;
    uint32_t addr = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == 'q' || line[0] == 'Q') break;
        if (line[0] == '@') { addr = static_cast<uint32_t>(std::strtoul(line.c_str() + 1, nullptr, 16)); continue; }
        std::istringstream bytes(line);
        std::string b;
        while (bytes >> b) {
            if (b.size() != 2 || hexval(b[0]) < 0 || hexval(b[1]) < 0) { why = "TI-TXT: '" + b + "' is not a hex byte"; return false; }
            m[addr++] = static_cast<uint8_t>(hexval(b[0]) << 4 | hexval(b[1]));
        }
    }
    img.format = "ti-txt";
    runsToSections(m, img);
    return true;
}

// Intel hex (hex6x -i): ":LLAAAATT data CC" records; 02 and 04 records set the upper address bits.
static bool loadIntel(const std::string &text, Image &img, std::string &why) {
    std::map<uint32_t, uint8_t> m;
    std::istringstream in(text);
    std::string line;
    uint32_t upper = 0;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<uint8_t> h;
        if (line[0] != ':' || !hexBytes(line, 1, (line.size() - 1) / 2, h) || h.size() < 5 || h.size() != size_t(h[0]) + 5) {
            why = "Intel hex: line " + std::to_string(n) + " is not a record"; return false;
        }
        uint8_t sum = 0;
        for (uint8_t b : h) sum = static_cast<uint8_t>(sum + b);
        if (sum != 0) { why = "Intel hex: line " + std::to_string(n) + " fails its checksum"; return false; }
        uint32_t a = (h[1] << 8) | h[2];
        switch (h[3]) {
        case 0: addBytes(m, upper + a, std::vector<uint8_t>(h.begin() + 4, h.end() - 1)); break;
        case 1: goto done;
        case 2: upper = ((h[4] << 8) | h[5]) << 4; break;
        case 4: upper = ((h[4] << 8) | h[5]) << 16; break;
        case 3: case 5: img.entry = (h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7]; img.hasEntry = true; break;
        default: break;
        }
    }
done:
    img.format = "intel";
    runsToSections(m, img);
    return true;
}

// Motorola S-records (hex6x -m): S1/S2/S3 data with 16-, 24- or 32-bit addresses; S7/S8/S9 the entry.
static bool loadSrec(const std::string &text, Image &img, std::string &why) {
    std::map<uint32_t, uint8_t> m;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 4) continue;
        std::vector<uint8_t> h;
        if (line[0] != 'S' || !hexBytes(line, 2, (line.size() - 2) / 2, h) || h.empty() || h.size() != size_t(h[0]) + 1) {
            why = "S-record: line " + std::to_string(n) + " is not a record"; return false;
        }
        int t = line[1] - '0';
        int alen = t == 1 || t == 9 ? 2 : t == 2 || t == 8 ? 3 : t == 3 || t == 7 ? 4 : 0;
        if (alen == 0) continue;
        uint32_t a = 0;
        for (int i = 0; i < alen; i++) a = (a << 8) | h[1 + i];
        if (t <= 3) addBytes(m, a, std::vector<uint8_t>(h.begin() + 1 + alen, h.end() - 1));
        else { img.entry = a; img.hasEntry = true; }
    }
    img.format = "srec";
    runsToSections(m, img);
    return true;
}

bool loadImage(const std::string &path, Image &img, std::string &why, bool rawBinary, uint32_t binBase) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) { why = "cannot open " + path; return false; }
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (rawBinary) {
        Section s; s.name = ".bin"; s.addr = binBase; s.bytes = f; s.size = static_cast<uint32_t>(f.size()); s.code = true;
        img.sections.push_back(s);
        img.format = "bin";
        img.entry = binBase; img.hasEntry = true;
        return true;
    }
    if (f.size() >= 4 && f[0] == 0x7f && f[1] == 'E' && f[2] == 'L' && f[3] == 'F') return loadElf(f, img, why);
    if (f.size() >= 2 && u16(f, 0) == 0x00c2) return loadCoff(f, img, why);
    std::string text(f.begin(), f.end());
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { why = path + " is empty"; return false; }
    if (text[first] == '@') return loadTiTxt(text.substr(first), img, why);
    if (text[first] == ':') return loadIntel(text.substr(first), img, why);
    if (text[first] == 'S') return loadSrec(text.substr(first), img, why);
    why = path + ": not an ELF or COFF file, nor a TI-TXT, Intel or Motorola hex image (a raw binary wants --bin ADDR)";
    return false;
}

} // namespace c6x
