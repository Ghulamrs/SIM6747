#pragma once

// --debug: a debugger session over stdin and stdout, in gdb's words - the subset RIDE's Debug session
// speaks to gdb (break FILE:LINE, run, continue, next, step, finish, backtrace, frame, print, delete,
// echo, quit) answered in gdb's shapes, so that an editor built to read gdb reads this unchanged. One
// shell, written once, over the Machine below: vm6747 runs the assembly text under it, sim6747 the
// linked image (its src/Debug.* is this file, copied).

#include <cstdint>
#include <string>
#include <vector>

namespace dbg {

// Where an instruction came from: a file as the compiler named it, a line, or no line at all (0).
struct Position {
    std::string file;
    int line = 0;
    bool known() const { return line > 0; }
    bool operator==(const Position &o) const { return line == o.line && file == o.file; }
    bool operator!=(const Position &o) const { return !(*this == o); }
};

// What the shell asks of a target. Registers are numbered 0-31 A0-A31, 32-63 B0-B31; a value read is
// the one a halted debugger shows, every result in flight landed, and reading moves nothing.
class Machine {
public:
    virtual ~Machine() {}
    virtual std::string programName() const = 0;
    virtual uint32_t pc() const = 0;
    virtual void step() = 0;                  // one execute packet, or one native library call
    virtual bool ended() const = 0;           // the program has exited
    virtual int status() const = 0;           // its exit status, once ended
    virtual void finish() = 0;                // once ended: what runs after main (atexit handlers)
    virtual bool hasLines() const = 0;        // -g: any instruction with a source position
    virtual std::string noLinesWhy() const = 0;
    virtual bool positionAt(uint32_t pc, Position &p) const = 0;
    // The first address of LINE in the file named by its leaf, or of the nearest line after it that has
    // code; lineUsed says which.
    virtual bool addressOfLine(const std::string &leaf, int line, uint32_t &addr, int &lineUsed) const = 0;
    virtual bool functionAt(uint32_t pc, std::string &name, uint32_t &entry) const = 0;
    virtual bool addressOfFunction(const std::string &name, uint32_t &addr) const = 0;
    virtual bool isFunctionEntry(uint32_t pc) const = 0;
    virtual uint32_t reg(int r) const = 0;
    virtual void setReg(int r, uint32_t v) = 0;
    // The text of a source line, for the echo gdb prints after a stop - or empty.
    virtual std::string sourceLine(const std::string &file, int line) = 0;
};

// Run the session: commands from stdin until quit or end of input. The exit status to return.
int shell(Machine &m);

}  // namespace dbg
