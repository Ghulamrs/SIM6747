// The emulator under Debug.h's shell: the program's instructions carry the `.dwpsn` position the
// assembler stamped on them (Program.h), a function is any label that is not one of the compilers'
// own (L$f$n, L.f.n, $C$L1 ...), and the stack is the shell's. --debug in main.cpp.

#include "DebugVm.h"

#include "Debug.h"

#include "Cpu.h"
#include "Runtime.h"

#include <fstream>
#include <map>

namespace {

std::string leafOf(const std::string &path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

bool functionName(const std::string &n) {
    if (n.empty()) return false;
    if (n[0] == '$' || n[0] == '.') return false;
    if (n.compare(0, 2, "L$") == 0 || n.compare(0, 2, "L.") == 0) return false;
    return true;
}

class VmMachine : public dbg::Machine {
public:
    VmMachine(Cpu &cpu, Program &prog, Runtime &rt, const std::string &name) : cpu_(cpu), prog_(prog), rt_(rt), name_(name) {
        for (std::map<uint32_t, std::string>::const_iterator n = prog_.names.begin(); n != prog_.names.end(); ++n)
            if (functionName(n->second) && prog_.code.count(n->first)) functions_[n->first] = n->second;
        for (std::map<uint32_t, Instr>::const_iterator i = prog_.code.begin(); i != prog_.code.end(); ++i)
            if (i->second.srcLine > 0) { lines_ = true; break; }
    }
    std::string programName() const override { return name_; }
    uint32_t pc() const override { return cpu_.pc(); }
    void step() override { cpu_.debugStep(); }
    bool ended() const override { return !cpu_.running(); }
    int status() const override { return cpu_.exitCode(); }
    void finish() override { if (!finished_) { finished_ = true; rt_.runAtExit(cpu_); } }
    bool hasLines() const override { return lines_; }
    std::string noLinesWhy() const override { return "the assembly carries no .dwpsn - compile with -g"; }
    bool positionAt(uint32_t pc, dbg::Position &p) const override {
        std::map<uint32_t, Instr>::const_iterator i = prog_.code.find(pc);
        if (i == prog_.code.end()) return false;
        p.file = i->second.srcFile; p.line = i->second.srcLine;
        return true;
    }
    bool addressOfLine(const std::string &leaf, int line, uint32_t &addr, int &lineUsed) const override {
        bool found = false;
        for (std::map<uint32_t, Instr>::const_iterator i = prog_.code.begin(); i != prog_.code.end(); ++i) {
            const Instr &in = i->second;
            if (in.srcLine < line || leafOf(in.srcFile) != leaf) continue;
            if (!found || in.srcLine < lineUsed) { found = true; lineUsed = in.srcLine; addr = i->first; }
        }
        return found;
    }
    bool functionAt(uint32_t pc, std::string &name, uint32_t &entry) const override {
        std::map<uint32_t, std::string>::const_iterator f = functions_.upper_bound(pc);
        if (f == functions_.begin()) return false;
        --f;
        name = f->second; entry = f->first;
        return true;
    }
    bool addressOfFunction(const std::string &name, uint32_t &addr) const override {
        for (std::map<uint32_t, std::string>::const_iterator f = functions_.begin(); f != functions_.end(); ++f)
            if (f->second == name) { addr = f->first; return true; }
        return false;
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
            std::string l;
            while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); text.push_back(l); }
            f = sources_.insert(std::make_pair(file, text)).first;
        }
        if (line < 1 || static_cast<size_t>(line) > f->second.size()) return std::string();
        return f->second[static_cast<size_t>(line) - 1];
    }

private:
    Cpu &cpu_;
    Program &prog_;
    Runtime &rt_;
    std::string name_;
    std::map<uint32_t, std::string> functions_;
    std::map<std::string, std::vector<std::string> > sources_;
    bool lines_ = false, finished_ = false;
};

}  // namespace

int debugRun(Cpu &cpu, Program &prog, Runtime &rt, uint32_t entry, const std::string &name) {
    rt.withNoInput();
    cpu.startAt(entry);
    VmMachine m(cpu, prog, rt, name);
    return dbg::shell(m);
}
