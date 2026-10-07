// sim6747 --dis: the machine code of a C6000 image listed as TI's assembler would write it, a fetch packet at a
// time, so that the decoder can be held to TI's dis6x on the same file (oracle/compare-dis.py).
//   sim6747 --dis file.out|file.obj|image.hex [--all]       code sections, or with --all every loaded byte
//   sim6747 --dis --bin ADDR file.bin
//   sim6747 --dis-words ADDR WORD...                        words given on the command line, from ADDR

#include "C6xDis.h"
#include "C6xImage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace c6x {

static void listRange(const Image &img, uint32_t from, uint32_t to) {
    uint32_t fp = from & ~31u;
    bool startPrinted = false;
    for (; fp < to; fp += 32) {
        std::vector<Insn> v = decodeFetchPacket(img, fp);
        bool prevParallel = startPrinted ? false : parallelWithPrevious(img, fp);
        for (size_t i = 0; i < v.size(); i++) {
            const Insn &in = v[i];
            bool par = i > 0 ? v[i - 1].parallel && !v[i - 1].header : prevParallel;
            if (in.addr < from || in.addr >= to) { prevParallel = in.parallel; continue; }
            std::map<uint32_t, std::string>::const_iterator s = img.symbolsAt.find(in.addr);
            if (s != img.symbolsAt.end()) std::printf("%08x %s:\n", in.addr, s->second.c_str());
            if (in.size == 2) std::printf("%08x     %04x  %s%s\n", in.addr, in.word & 0xffff, par ? "|| " : "   ", formatInsn(img, in).c_str());
            else std::printf("%08x %08x  %s%s\n", in.addr, in.word, par && !in.header ? "|| " : "   ", formatInsn(img, in).c_str());
            startPrinted = true;
        }
        if (!v.empty()) prevParallel = v.back().parallel;
    }
}

int disMain(int argc, char **argv) {
    // argv[0] is "--dis" or "--dis-words"
    if (std::strcmp(argv[0], "--dis-words") == 0) {
        if (argc < 3) { std::fprintf(stderr, "usage: sim6747 --dis-words ADDR WORD...\n"); return 2; }
        Image img;
        Section s; s.name = ".words"; s.addr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 16)); s.code = true;
        for (int i = 2; i < argc; i++) {
            uint32_t w = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 16));
            for (int b = 0; b < 4; b++) s.bytes.push_back(static_cast<uint8_t>(w >> (8 * b)));
        }
        s.size = static_cast<uint32_t>(s.bytes.size());
        img.sections.push_back(s);
        listRange(img, s.addr, s.addr + s.size);
        return 0;
    }
    bool all = false, bin = false;
    uint32_t base = 0;
    std::string path;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--all") == 0) all = true;
        else if (std::strcmp(argv[i], "--bin") == 0 && i + 1 < argc) { bin = true; base = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16)); }
        else path = argv[i];
    }
    if (path.empty()) { std::fprintf(stderr, "usage: sim6747 --dis [--all] [--bin ADDR] file\n"); return 2; }
    Image img;
    std::string why;
    if (!loadImage(path, img, why, bin, base)) { std::fprintf(stderr, "sim6747: %s\n", why.c_str()); return 1; }
    std::printf("; %s: %s image, %zu sections", path.c_str(), img.format.c_str(), img.sections.size());
    if (img.hasEntry) std::printf(", entry 0x%08x", img.entry);
    std::printf("\n");
    for (const Section &s : img.sections) {
        if (s.bytes.empty() || (!s.code && !all)) continue;
        std::printf("\n; section %s at 0x%08x, %u bytes\n", s.name.c_str(), s.addr, s.size);
        listRange(img, s.addr, s.addr + static_cast<uint32_t>(s.bytes.size()));
    }
    return 0;
}

} // namespace c6x
