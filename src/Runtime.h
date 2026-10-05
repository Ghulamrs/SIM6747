#pragma once

// The C library and the EABI helpers, provided natively: a call to one of these names lands on a stub below the text
// base and is answered here, reading its arguments by the C6000 convention the compilers emit (README.md, "The C
// library, natively") and returning in A4 (A5:A4). Memory the library hands out comes from a heap above the program's data.

#include <cstdint>
#include <string>
#include <vector>

class Cpu;

class Runtime {
public:
    static std::vector<std::string> names();
    bool call(const std::string &name, Cpu &cpu);
    void setHeap(uint32_t start, uint32_t end) { heap_ = start; heapEnd_ = end; }
    // The assembly the runtime contributes: the streams, the C++ ABI's
    // typeinfo vtables, __dso_handle.
    static std::string prelude();
    static std::string fundamentalTypeInfos();
    // cl6x's <iostream>: cout, cerr and clog laid out as STLport's basic_ostream, each over a
    // streambuf and a num_put facet whose virtuals are natives (README.md, "cl6x's streams").
    static std::string streamObjects();
    // After main returns or exit is called: the __cxa_atexit registrations, last first.
    void runAtExit(Cpu &cpu);

private:
    uint32_t heap_ = 0, heapEnd_ = 0;
    // A streambuf the runtime made carries its stream number at +32; a num_put virtual pads its
    // text by the ios_base's width and adjustfield, writes it there, and answers the iterator in A5:A4.
    FILE *hostOf(Cpu &cpu, uint32_t sb);
    void numPut(Cpu &cpu, const std::string &text, bool hexPrefixed);
    struct Block { uint32_t at, size; bool used; };
    std::vector<Block> blocks_;
    uint32_t allocate(Cpu &cpu, uint32_t size);
    void release(Cpu &cpu, uint32_t at);

    // A cursor over arguments in memory, as va_list sees them.
    struct Args {
        Cpu &cpu; uint32_t at;
        uint32_t word();
        uint64_t wide();
        double dbl();
    };
    std::string format(Cpu &cpu, const std::string &fmt, Args &args);
    struct File { std::string path; std::string data; size_t pos; bool write; };
    std::vector<File> files_;
    File stdin_; bool stdinRead_ = false;
    File *streamFile(uint32_t stream);
    uint32_t streamNumber(Cpu &cpu, uint32_t stream);   // &_ftable[n] -> 1, 2, 3
    uint32_t handlers_[32] = { 0 };
    uint32_t errno_ = 0;
    bool closed_[3] = { false, false, false };   // close(0..2): what a closed standard stream swallows
    FILE *host(int fd) const;                    // stdout or stderr for fd 1 or 2, null once closed
    struct AtExit { uint32_t fn, arg; };
    std::vector<AtExit> atExit_;
    uint32_t dynamicCast(Cpu &cpu, uint32_t sub, uint32_t src, uint32_t dst);

    // Exceptions: TI's index, one entry per function - its address and the frame's compact unwind
    // word, or the address of its table - sorted so a return address finds its own; the exceptions
    // in flight or caught; and the unwinder, which reads the tables as TI's personality would.
    struct ExidxEntry { uint32_t func, word; };
    std::vector<ExidxEntry> exidx_;
    bool ehLoaded_ = false;
    void loadExidx(Cpu &cpu);
    const ExidxEntry *entryFor(uint32_t pc) const;
    bool callerOf(Cpu &cpu, uint32_t pc, uint32_t fp, uint32_t &callerPc, uint32_t &callerFp);
    struct Exc {
        uint32_t obj = 0, ti = 0, dtor = 0, adjusted = 0;
        int handlers = 0;          // __cxa_begin_catch calls outstanding
        bool rethrown = false;
        uint32_t barrierFp = 0, barrierDesc = 0;    // phase two's destination: the frame and the descriptor
        uint32_t cleanupPc = 0, cleanupNext = 0;    // where a cleanup pad's _Unwind_Resume carries on
    };
    std::vector<Exc> excs_;        // every exception allocated and not yet freed
    std::vector<uint32_t> caught_; // the stack of exceptions being handled
    uint32_t cleanupExc_ = 0;      // the exception whose cleanup pad is running: TI's cleanup_exception
    Exc *excFor(uint32_t obj);
    bool matches(Cpu &cpu, uint32_t obj, uint32_t thrownTi, uint32_t catchTi, uint32_t &adjusted);
    void throwFrom(Cpu &cpu, uint32_t obj, uint32_t pc, uint32_t fp, uint32_t sp);
    void unwindTo(Cpu &cpu, Exc &e, uint32_t pc, uint32_t fp, uint32_t sp, uint32_t from);
    uint32_t descriptors(Cpu &cpu, const ExidxEntry &e);
    void land(Cpu &cpu, uint32_t fp, uint32_t sp, uint32_t obj, uint32_t pad, bool withObject);
    void popSaved(Cpu &cpu, uint32_t pc, uint32_t fp);
    [[noreturn]] void terminate(Cpu &cpu, const char *why);
    uint32_t errnoAt(Cpu &cpu);
    void setErrno(Cpu &cpu, uint32_t v);
};
