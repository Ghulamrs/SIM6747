// The linked image under Debug.h's shell. Lines: the DWARF line program (versions 2 to 4) of the image's
// .debug_line, run unit by unit into rows of (address, file, line) - TI's cl6x writes version 3 with a
// minimum instruction length of 2, the compact instructions' - and a program counter's position is the
// last row at or before it within its sequence. Functions: the ELF's STT_FUNC symbols; a COFF image, or
// one with none, falls back to every code symbol that is not a local label.

#include "DebugC6x.h"

#include "Debug.h"

#include <fstream>
#include <map>
#include <vector>

namespace c6x {

namespace {

std::string leafOf(const std::string &path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

std::string folderOf(const std::string &path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? std::string() : path.substr(0, k + 1);
}

struct Row { uint32_t addr; int file; int line; bool stmt; uint32_t seqEnd; };

// The line table read out of .debug_line: rows by address (the last row at an address wins), and the
// file names each row's file index names, one list across every unit.
struct LineTable {
    std::map<uint32_t, Row> rows;
    std::vector<std::string> files;
    bool read(const std::vector<uint8_t> &d);
};

struct Reader {
    const std::vector<uint8_t> &d;
    size_t at, end;
    bool ok = true;
    uint8_t u8() { if (at >= end) { ok = false; return 0; } return d[at++]; }
    uint16_t u16() { uint16_t v = u8(); return static_cast<uint16_t>(v | (u8() << 8)); }
    uint32_t u32() { uint32_t v = u16(); return v | (uint32_t(u16()) << 16); }
    uint64_t uleb() { uint64_t v = 0; int sh = 0; for (;;) { uint8_t b = u8(); v |= uint64_t(b & 0x7f) << sh; sh += 7; if (!(b & 0x80) || !ok || sh > 63) break; } return v; }
    int64_t sleb() { int64_t v = 0; int sh = 0; uint8_t b; do { b = u8(); v |= int64_t(b & 0x7f) << sh; sh += 7; } while ((b & 0x80) && ok && sh < 64); if (sh < 64 && (b & 0x40)) v |= -(int64_t(1) << sh); return v; }
    std::string str() { std::string s; while (at < end && d[at]) s += static_cast<char>(d[at++]); if (at < end) at++; else ok = false; return s; }
};

bool LineTable::read(const std::vector<uint8_t> &d) {
    size_t at = 0;
    bool any = false;
    while (at + 4 <= d.size()) {
        Reader hdr{d, at, d.size()};
        uint32_t unitLen = hdr.u32();
        if (unitLen == 0xffffffffu || !hdr.ok) break;                    // 64-bit DWARF: not read
        size_t unitEnd = hdr.at + unitLen;
        if (unitEnd > d.size()) break;
        Reader r{d, hdr.at, unitEnd};
        uint16_t version = r.u16();
        if (version < 2 || version > 4) { at = unitEnd; continue; }
        uint32_t headerLen = r.u32();
        size_t programAt = r.at + headerLen;
        int minInst = r.u8();
        if (version >= 4) r.u8();                                          // maximum operations per instruction
        bool defaultStmt = r.u8() != 0;
        int lineBase = static_cast<int8_t>(r.u8());
        int lineRange = r.u8();
        int opcodeBase = r.u8();
        std::vector<int> stdLengths(static_cast<size_t>(opcodeBase > 0 ? opcodeBase : 1), 0);
        for (int k = 1; k < opcodeBase; k++) stdLengths[static_cast<size_t>(k)] = r.u8();
        std::vector<std::string> dirs;
        dirs.push_back(std::string());
        for (;;) { std::string s = r.str(); if (s.empty() || !r.ok) break; dirs.push_back(s); }
        std::vector<int> fileIndex;                                        // the unit's file numbers, 1-based, into files
        fileIndex.push_back(-1);
        for (;;) {
            std::string s = r.str();
            if (s.empty() || !r.ok) break;
            uint64_t dir = r.uleb(); r.uleb(); r.uleb();
            std::string full = s;
            if (dir != 0 && dir < dirs.size() && s.find_first_of("/\\") == std::string::npos && s.find(':') == std::string::npos)
                full = dirs[static_cast<size_t>(dir)] + "/" + s;
            fileIndex.push_back(static_cast<int>(files.size()));
            files.push_back(full);
        }
        if (!r.ok || lineRange == 0 || minInst == 0) { at = unitEnd; continue; }
        // The state machine, DWARF 6.2.2: every row appended as the program emits it.
        Reader p{d, programAt, unitEnd};
        uint32_t addr = 0; int file = 1, line = 1; bool stmt = defaultStmt;
        std::vector<Row> seq;
        auto emit = [&]() {
            Row row; row.addr = addr; row.file = file > 0 && static_cast<size_t>(file) < fileIndex.size() ? fileIndex[static_cast<size_t>(file)] : -1;
            row.line = line; row.stmt = stmt; row.seqEnd = 0;
            seq.push_back(row);
        };
        auto endSequence = [&]() {
            emit();
            uint32_t endAt = seq.back().addr;
            for (size_t k = 0; k + 1 < seq.size(); k++) { seq[k].seqEnd = endAt; rows[seq[k].addr] = seq[k]; any = true; }
            seq.clear();
            addr = 0; file = 1; line = 1; stmt = defaultStmt;
        };
        while (p.ok && p.at < unitEnd) {
            uint8_t op = p.u8();
            if (op >= opcodeBase) {
                int adj = op - opcodeBase;
                addr += static_cast<uint32_t>((adj / lineRange) * minInst);
                line += lineBase + adj % lineRange;
                emit();
            } else if (op == 0) {
                uint64_t len = p.uleb();
                size_t next = p.at + len;
                uint8_t ext = p.u8();
                if (ext == 1) endSequence();
                else if (ext == 2) addr = p.u32();
                else if (ext == 3) { p.str(); p.uleb(); p.uleb(); p.uleb(); }
                p.at = next;
            } else if (op == 1) emit();
            else if (op == 2) addr += static_cast<uint32_t>(p.uleb() * static_cast<uint64_t>(minInst));
            else if (op == 3) line += static_cast<int>(p.sleb());
            else if (op == 4) file = static_cast<int>(p.uleb());
            else if (op == 5) p.uleb();
            else if (op == 6) stmt = !stmt;
            else if (op == 7) {}
            else if (op == 8) addr += static_cast<uint32_t>(((255 - opcodeBase) / lineRange) * minInst);
            else if (op == 9) addr += p.u16();
            else if (op == 10 || op == 11) {}
            else if (op == 12) p.uleb();
            else for (int k = 0; k < stdLengths[static_cast<size_t>(op)]; k++) p.uleb();
        }
        at = unitEnd;
    }
    return any;
}

class C6xMachine : public dbg::Machine {
public:
    C6xMachine(Cpu6x &cpu, const Image &img, const std::string &path, const DebugEnds &ends)
        : cpu_(cpu), img_(img), path_(path), ends_(ends) {
        lines_ = !img.debugLine.empty() && table_.read(img.debugLine);
        // cl6x marks its call-return labels ($C$RL0) and local labels ($C$L1) STT_FUNC as well: a function is
        // a code symbol whose name is not one of those.
        for (std::map<uint32_t, std::string>::const_iterator s = img_.symbolsAt.begin(); s != img_.symbolsAt.end(); ++s) {
            const std::string &n = s->second;
            if (n.empty() || n[0] == '$' || n[0] == '.' || n.compare(0, 2, "L$") == 0 || n.compare(0, 2, "L.") == 0) continue;
            if (img_.functions.empty() ? inCode(s->first) : img_.functions.count(s->first) != 0) functions_.insert(s->first);
        }
        // -g: each function's variables from the image's .gride.dwtag, by the address its label names
        std::map<std::string, std::vector<dbg::Variable> > byLabel;
        dbg::readTags(img_.dwTags, byLabel, types_);
        for (std::map<std::string, std::vector<dbg::Variable> >::const_iterator t = byLabel.begin(); t != byLabel.end(); ++t) {
            uint32_t at = 0;
            if (!addressOfFunction(t->first, at)) continue;
            variables_[at] = t->second;
            dbg::placeBlocks(variables_[at], [this](const std::string &n, uint32_t &a) { return addressOfFunction(n, a); });
        }
    }
    const dbg::TypeInfo *typeNamed(const std::string &name) const override {
        std::map<std::string, dbg::TypeInfo>::const_iterator t = types_.find(name);
        return t == types_.end() ? nullptr : &t->second;
    }
    const std::vector<dbg::Variable> *variablesAt(uint32_t entry) const override {
        std::map<uint32_t, std::vector<dbg::Variable> >::const_iterator v = variables_.find(entry);
        return v == variables_.end() ? nullptr : &v->second;
    }
    bool read(uint32_t addr, uint8_t *buf, size_t n) const override {
        cpu_.flushStores();   // a store still in flight is part of what a halted debugger shows
        for (size_t k = 0; k < n; k++) {
            const uint32_t a = addr + static_cast<uint32_t>(k);
            if (!cpu_.memory().mapped(a)) return false;
            buf[k] = cpu_.memory().read8(a);
        }
        return true;
    }
    bool write(uint32_t addr, const uint8_t *buf, size_t n) override {
        cpu_.flushStores();
        for (size_t k = 0; k < n; k++) if (!cpu_.memory().mapped(addr + static_cast<uint32_t>(k))) return false;
        for (size_t k = 0; k < n; k++) cpu_.memory().write8(addr + static_cast<uint32_t>(k), buf[k]);
        return true;
    }
    std::string programName() const override { return path_; }
    uint32_t pc() const override { return cpu_.pc(); }
    void step() override { cpu_.step(); }
    bool ended() const override { return ends_.ended(); }
    int status() const override { return ends_.status(); }
    void finish() override {}
    bool hasLines() const override { return lines_; }
    std::string noLinesWhy() const override {
        if (img_.debugLine.empty())
            return "the image carries no .debug_line - cl6x -g writes one; cpp11 -g writes .dwpsn, which ASM6x does not "
                   "yet turn into a line table and LNK6x does not yet carry into an image";
        return "the image's .debug_line could not be read";
    }
    bool startsRow(uint32_t pc) const override { return lines_ && table_.rows.count(pc) != 0; }
    bool positionAt(uint32_t pc, dbg::Position &p) const override {
        p = dbg::Position();
        if (!lines_) return inCode(pc);
        std::map<uint32_t, Row>::const_iterator r = table_.rows.upper_bound(pc);
        if (r == table_.rows.begin()) return inCode(pc);
        --r;
        if (pc >= r->second.seqEnd || r->second.file < 0) return inCode(pc);
        p.file = table_.files[static_cast<size_t>(r->second.file)];
        p.line = r->second.line;
        return true;
    }
    bool addressOfLine(const std::string &leaf, int line, uint32_t &addr, int &lineUsed) const override {
        bool found = false;
        for (std::map<uint32_t, Row>::const_iterator r = table_.rows.begin(); r != table_.rows.end(); ++r) {
            const Row &row = r->second;
            if (!row.stmt || row.line < line || row.file < 0 || leafOf(table_.files[static_cast<size_t>(row.file)]) != leaf) continue;
            if (!found || row.line < lineUsed) { found = true; lineUsed = row.line; addr = row.addr; }
        }
        return found;
    }
    bool functionAt(uint32_t pc, std::string &name, uint32_t &entry) const override {
        std::set<uint32_t>::const_iterator f = functions_.upper_bound(pc);
        if (f == functions_.begin()) return false;
        --f;
        entry = *f;
        std::map<uint32_t, std::string>::const_iterator n = img_.symbolsAt.find(entry);
        name = n == img_.symbolsAt.end() ? "??" : n->second;
        return true;
    }
    bool addressOfFunction(const std::string &name, uint32_t &addr) const override {
        std::map<std::string, uint32_t>::const_iterator s = img_.symbols.find(name);
        if (s == img_.symbols.end() && name[0] != '_') s = img_.symbols.find("_" + name);
        if (s == img_.symbols.end()) return false;
        addr = s->second;
        return true;
    }
    bool isFunctionEntry(uint32_t pc) const override { return functions_.count(pc) != 0; }
    uint32_t reg(int r) const override { return cpu_.landedReg(r); }
    void setReg(int r, uint32_t v) override { cpu_.setReg(r, v); }
    std::string sourceLine(const std::string &file, int line) override {
        std::map<std::string, std::vector<std::string> >::iterator f = sources_.find(file);
        if (f == sources_.end()) {
            std::vector<std::string> text;
            std::ifstream in(file.c_str());
            if (!in) in.open(leafOf(file).c_str());
            if (!in) in.open((folderOf(path_) + leafOf(file)).c_str());
            std::string l;
            while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); text.push_back(l); }
            f = sources_.insert(std::make_pair(file, text)).first;
        }
        if (line < 1 || static_cast<size_t>(line) > f->second.size()) return std::string();
        return f->second[static_cast<size_t>(line) - 1];
    }

private:
    Cpu6x &cpu_;
    const Image &img_;
    std::string path_;
    DebugEnds ends_;
    LineTable table_;
    bool lines_ = false;
    std::set<uint32_t> functions_;
    std::map<uint32_t, std::vector<dbg::Variable> > variables_;
    std::map<std::string, dbg::TypeInfo> types_;
    std::map<std::string, std::vector<std::string> > sources_;
    bool inCode(uint32_t a) const {
        for (const Section &s : img_.sections) if (s.code && a >= s.addr && a < s.addr + s.size) return true;
        return false;
    }
};

} // namespace

int debugRun(Cpu6x &cpu, const Image &img, const std::string &path, const DebugEnds &ends) {
    C6xMachine m(cpu, img, path, ends);
    return dbg::shell(m);
}

} // namespace c6x
