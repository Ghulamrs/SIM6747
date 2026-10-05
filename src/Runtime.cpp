#include "Runtime.h"
#include "Cpu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ---- the argument cursor ------------------------------------------------------
uint32_t Runtime::Args::word() { uint32_t v = cpu.load32(at); at += 4; return v; }
uint64_t Runtime::Args::wide() { at = (at + 7) & ~7u; uint64_t v = cpu.load64(at); at += 8; return v; }
double Runtime::Args::dbl() { uint64_t v = wide(); double d; std::memcpy(&d, &v, 8); return d; }

// The registers an argument arrives in, by position, and where the stack
// part begins: B15 + 4 is the first stack word.
static const int kArgRegs[10] = { 4, Cpu::B + 4, 6, Cpu::B + 6, 8, Cpu::B + 8, 10, Cpu::B + 10, 12, Cpu::B + 12 };
static uint32_t arg(Cpu &c, int i) {
    if (i < 10) return c.reg(kArgRegs[i]);
    return c.load32(c.reg(Cpu::B15) + 4 + 4 * (i - 10));
}
static uint64_t argWide(Cpu &c, int i) {
    if (i < 10) return c.pair(kArgRegs[i]);
    return c.load64(c.reg(Cpu::B15) + 4 + 4 * (i - 10));
}
static double argDouble(Cpu &c, int i) { uint64_t v = argWide(c, i); double d; std::memcpy(&d, &v, 8); return d; }
static float argFloat(Cpu &c, int i) { uint32_t v = arg(c, i); float f; std::memcpy(&f, &v, 4); return f; }
// A variadic call: the last named argument is on the stack; this is the
// cursor at it, given how many named arguments there are.
static uint32_t variadicAt(Cpu &c, int named) {
    return c.reg(Cpu::B15) + 4;
    (void)named;
}
static void ret(Cpu &c, uint32_t v) { c.setReg(Cpu::A4, v); }
static void retWide(Cpu &c, uint64_t v) { c.setPair(Cpu::A4, v); }
// A NaN goes back as the canonical quiet NaN, sign clear, as the CPU's own
// arithmetic answers it - not as the host happened to produce it.
static void retDouble(Cpu &c, double d) { uint64_t v; if (d != d) v = 0x7ff8000000000000ULL; else std::memcpy(&v, &d, 8); retWide(c, v); }
static void retFloat(Cpu &c, float f) { uint32_t v; if (f != f) v = 0x7fc00000u; else std::memcpy(&v, &f, 4); ret(c, v); }

// ---- streams: 1 is stdin, read whole on first use; 4 onward are fopen's ------
// A program built with lib/stdio.h's C6000 branch names them as TI does,
// `&_ftable[n]` with TI's 24-byte FILE; those addresses read as 1, 2, 3.
uint32_t Runtime::streamNumber(Cpu &c, uint32_t s) {
    std::map<std::string, uint32_t>::const_iterator t = c.program().symbols.find("_ftable");
    if (t != c.program().symbols.end() && s >= t->second && s < t->second + 3 * 24 &&
        (s - t->second) % 24 == 0)
        return 1 + (s - t->second) / 24;
    return s;
}

Runtime::File *Runtime::streamFile(uint32_t s) {
    if (s == 1) {
        if (!stdinRead_) {
            stdinRead_ = true;
            char b[4096]; size_t k;
            while ((k = std::fread(b, 1, sizeof b, stdin)) > 0) stdin_.data.append(b, k);
            stdin_.pos = 0; stdin_.write = false;
        }
        return &stdin_;
    }
    if (s >= 4 && s - 4 < files_.size()) return &files_[s - 4];
    return nullptr;
}

// ---- errno: one word on the heap, handed out by address -----------------------
uint32_t Runtime::errnoAt(Cpu &cpu) {
    if (errno_ == 0) errno_ = allocate(cpu, 4);
    return errno_;
}
void Runtime::setErrno(Cpu &cpu, uint32_t v) { cpu.store32(errnoAt(cpu), v); }

// ---- the heap ------------------------------------------------------------------
uint32_t Runtime::allocate(Cpu &cpu, uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 7) & ~7u;
    for (Block &b : blocks_)
        if (!b.used && b.size >= size) { b.used = true; return b.at; }
    if (heap_ + size > heapEnd_) cpu.fault("the heap is exhausted");
    Block b; b.at = heap_; b.size = size; b.used = true;
    blocks_.push_back(b);
    heap_ += size;
    return b.at;
}
void Runtime::release(Cpu &cpu, uint32_t at) {
    if (at == 0) return;
    for (Block &b : blocks_) if (b.at == at) { if (!b.used) cpu.fault("free of a block already freed"); b.used = false; return; }
    cpu.fault("free of a pointer malloc did not return");
}

// ---- printf ----------------------------------------------------------------------
std::string Runtime::format(Cpu &cpu, const std::string &fmt, Args &args) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '*') { spec += std::to_string(static_cast<int32_t>(args.word())); i++; }
        while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '.') {
            spec += fmt[i++];
            if (i < fmt.size() && fmt[i] == '*') { spec += std::to_string(static_cast<int32_t>(args.word())); i++; }
            while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) spec += fmt[i++];
        }
        int longs = 0;
        bool shortInt = false, charInt = false;
        while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i])) {
            if (fmt[i] == 'l' || fmt[i] == 'q' || fmt[i] == 'j') longs++;
            else if (fmt[i] == 'h') { if (shortInt) charInt = true; shortInt = true; }
            i++;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (longs >= 2) v = static_cast<long long>(args.wide());
            else { int32_t w = static_cast<int32_t>(args.word()); v = charInt ? static_cast<signed char>(w) : shortInt ? static_cast<short>(w) : w; }
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            if (longs >= 2) v = args.wide();
            else { uint32_t w = args.word(); v = charInt ? static_cast<unsigned char>(w) : shortInt ? static_cast<unsigned short>(w) : w; }
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
            break;
        }
        case 'c': std::snprintf(buf, sizeof buf, spec.append("c").c_str(), static_cast<int>(args.word())); break;
        case 's': {
            // Sized to the string: a 2000-byte line from a Shalimar program
            // was cut at 512 here, the one conversion whose width the
            // program chooses.
            uint32_t p = args.word();
            std::string s = p == 0 ? "(null)" : cpu.readString(p);
            std::vector<char> big(s.size() + 512);
            std::snprintf(big.data(), big.size(), spec.append("s").c_str(), s.c_str());
            out += big.data();
            continue;
        }
        case 'p': std::snprintf(buf, sizeof buf, "0x%x", args.word()); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            // A NaN or an infinity is spelt here, not by the host, whose
            // spelling differs (Windows writes -nan(ind)); the program's
            // output must not depend on where the emulator runs.
            double v = args.dbl();
            if (v != v || std::isinf(v)) {
                std::string word = v != v ? "nan" : "inf";
                if (std::isupper(static_cast<unsigned char>(conv))) for (char &ch : word) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (std::signbit(v)) word = "-" + word;
                else if (spec.find('+') != std::string::npos) word = "+" + word;
                else if (spec.find(' ') != std::string::npos) word = " " + word;
                std::string w = spec;                        // keep the width, drop 0 and the precision
                std::string flags, width;
                size_t k = 1;
                while (k < w.size() && std::strchr("-+ #0", w[k])) { if (w[k] == '-') flags += '-'; k++; }
                while (k < w.size() && std::isdigit(static_cast<unsigned char>(w[k]))) width += w[k++];
                std::snprintf(buf, sizeof buf, ("%" + flags + width + "s").c_str(), word.c_str());
                break;
            }
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
            break;
        }
        case 'n': { uint32_t p = args.word(); cpu.store32(p, static_cast<uint32_t>(out.size())); buf[0] = 0; break; }
        case '%': std::strcpy(buf, "%"); break;
        default: std::snprintf(buf, sizeof buf, "%s%c", spec.c_str(), conv); break;
        }
        out += buf;
    }
    return out;
}

// ---- what the runtime contributes as data ----------------------------------------
// The streams as objects; the Itanium ABI's three typeinfo classes' vtables, which a program's
// typeinfo objects point at (their address point, +8) and __dynamic_cast tells apart by the word
// there (1: no bases, 2: one base at offset 0, 3: several); std::type_info's own; and __dso_handle.
std::string Runtime::prelude() {
    return
        "\t.data\n"
        "\t.global stdin\n\t.global stdout\n\t.global stderr\n"
        "stdin:\t.word 1\nstdout:\t.word 2\nstderr:\t.word 3\n"
        "\t.global _ftable\n\t.align 8\n_ftable:\t.space 480\n"
        "\t.global __dso_handle\n__dso_handle:\t.word 0\n"
        "\t.global _ZTVN10__cxxabiv117__class_type_infoE\n"
        "_ZTVN10__cxxabiv117__class_type_infoE:\t.word 0, 0, 1, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVN10__cxxabiv120__si_class_type_infoE\n"
        "_ZTVN10__cxxabiv120__si_class_type_infoE:\t.word 0, 0, 2, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVN10__cxxabiv121__vmi_class_type_infoE\n"
        "_ZTVN10__cxxabiv121__vmi_class_type_infoE:\t.word 0, 0, 3, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVSt9type_info\n"
        "_ZTVSt9type_info:\t.word 0, 0, 0, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVN10__cxxabiv123__fundamental_type_infoE\n"
        "_ZTVN10__cxxabiv123__fundamental_type_infoE:\t.word 0, 0, 4, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVN10__cxxabiv119__pointer_type_infoE\n"
        "_ZTVN10__cxxabiv119__pointer_type_infoE:\t.word 0, 0, 5, 0, 0, 0, 0, 0\n"
        "\t.global _ZTVN10__cxxabiv116__enum_type_infoE\n"
        "_ZTVN10__cxxabiv116__enum_type_infoE:\t.word 0, 0, 6, 0, 0, 0, 0, 0\n"
        + fundamentalTypeInfos() + streamObjects();
}

// Each stream is STLport's basic_ostream as cl6x 8.2.2 lays it out: its vptr, whose table holds
// the basic_ios offset (4) at -12; then ios_base - vptr, flags (skipws|dec, unitbuf for cerr),
// state, modes, mask, precision 6, width, locale, callbacks, iwords, pwords - ctype, fill, rdbuf, tie.
std::string Runtime::streamObjects() {
    std::string s =
        "\t.data\n\t.align 8\n"
        // basic_streambuf<char>'s fifteen virtuals behind a two-word header: sync is the sixth,
        // xsputn, _M_xsputnc and overflow the twelfth to fourteenth; the rest are never reached.
        "__vm6747_sb_vtbl:\t.word 0, 0\n"
        "__vm6747_sb_virt:\t.word 0, 0, 0, 0, 0, __vm6747_sb_sync, 0, 0, 0, 0, 0, __vm6747_sb_xsputn, __vm6747_sb_xsputnc, __vm6747_sb_overflow, 0\n"
        "__vm6747_os_vtbl:\t.word 4, 0, 0\n"
        "__vm6747_os_virt:\t.word 0, 0\n"
        // num_put<char>'s virtuals: the destructor pair, then bool, long, unsigned long, double,
        // long double, long long, unsigned long long, const void *. The facet: vptr, refcount.
        "__vm6747_np_vtbl:\t.word 0, 0\n"
        "__vm6747_np_virt:\t.word 0, 0, __vm6747_num_put_bool, __vm6747_num_put_long, __vm6747_num_put_ulong, "
        "__vm6747_num_put_double, __vm6747_num_put_ldouble, __vm6747_num_put_llong, __vm6747_num_put_ullong, __vm6747_num_put_pointer\n"
        "\t.global __vm6747_num_put\n__vm6747_num_put:\t.word __vm6747_np_virt, 1\n"
        "\t.global __vm6747_num_put_id\n__vm6747_num_put_id:\t.word 1\n"
        "\t.global __vm6747_locale\n__vm6747_locale:\t.word 0\n";
    static const struct { const char *name, *stream; int flags; const char *tie; } streams[] = {
        { "_ZSt4cout", "2", 4104, "0" }, { "_ZSt4cerr", "3", 12296, "_ZSt4cout" }, { "_ZSt4clog", "3", 4104, "0" },
    };
    for (size_t i = 0; i < 3; i++) {
        std::string sb = std::string("__vm6747_sb_") + streams[i].name;
        // The streambuf: vptr, the six get and put pointers (none, so every character reaches
        // overflow and every run xsputn), the locale, and the stream number the natives write to.
        s += "\t.align 8\n" + sb + ":\t.word __vm6747_sb_virt, 0, 0, 0, 0, 0, 0, 0, " + streams[i].stream + "\n";
        s += "\t.align 8\n\t.global " + std::string(streams[i].name) + "\n" + streams[i].name + ":\t.word __vm6747_os_virt\n";
        s += "\t.word 0, " + std::to_string(streams[i].flags) + ", 0, 0, 0, 0, 6, 0, __vm6747_locale, 0, 0, 0, 0, 0, 0, 0\n";
        s += "\t.word 0, 32, " + sb + ", " + streams[i].tie + "\n";
    }
    return s;
}

// The typeinfo objects the standard library carries for the fundamental types, and for pointers
// to them, so that `throw 7` and `catch (const char *)` name something: _ZTIi is [vptr][name "i"],
// _ZTIPKc is [vptr][name "PKc"][flags][pointee _ZTIc]. Matched by name.
std::string Runtime::fundamentalTypeInfos() {
    static const char *const codes[] = { "v", "b", "c", "a", "h", "s", "t", "i", "j", "l", "m",
                                         "x", "y", "n", "o", "f", "d", "e", "w", "Dn" };
    std::string o;
    for (const char *code : codes) {
        std::string k = code;
        o += "\t.global _ZTS" + k + "\n_ZTS" + k + ":\t.cstring \"" + k + "\"\n";
        o += "\t.align 4\n\t.global _ZTI" + k + "\n_ZTI" + k + ":\t.word _ZTVN10__cxxabiv123__fundamental_type_infoE+8, _ZTS" + k + "\n";
        for (int q = 0; q < 2; q++) {
            std::string pk = (q ? "PK" : "P") + k;
            o += "\t.global _ZTS" + pk + "\n_ZTS" + pk + ":\t.cstring \"" + pk + "\"\n";
            o += "\t.align 4\n\t.global _ZTI" + pk + "\n_ZTI" + pk + ":\t.word _ZTVN10__cxxabiv119__pointer_type_infoE+8, _ZTS" + pk
               + ", " + (q ? "1" : "0") + ", _ZTI" + k + "\n";
        }
    }
    return o;
}

void Runtime::runAtExit(Cpu &cpu) {
    cpu.resume();
    for (size_t i = atExit_.size(); i-- > 0; ) cpu.callback(atExit_[i].fn, atExit_[i].arg, 0);
    atExit_.clear();
}

FILE *Runtime::host(int fd) const {
    if (fd < 1 || fd > 2 || closed_[fd]) return nullptr;
    return fd == 1 ? stdout : stderr;
}

// ---- cl6x's streams: what the num_put facet writes, by STLport's rules ----
FILE *Runtime::hostOf(Cpu &c, uint32_t sb) {
    uint32_t stream = sb == 0 ? 0 : c.load32(sb + 32);
    if (stream < 2 || stream > 3) c.fault("a streambuf the runtime did not make: cl6x's streams here are cout, cerr and clog");
    return host(static_cast<int>(stream) - 1);
}

static uint32_t symbolAt(Cpu &c, const char *name) {
    std::map<std::string, uint32_t>::const_iterator s = c.program().symbols.find(name);
    if (s == c.program().symbols.end()) c.fault(std::string("'") + name + "' is not in the runtime's prelude");
    return s->second;
}

// An integer as num_put writes it: hex and oct take the value's bits unsigned, with 0x or 0 under
// showbase; decimal takes a sign, or + under showpos; uppercase lifts the digits and the X.
static std::string integerText(int64_t v, bool isSigned, int bits, uint32_t flags) {
    const bool upper = (flags & 0x4000) != 0, showbase = (flags & 0x200) != 0;
    char buf[32];
    std::string s;
    if ((flags & 0x38) == 0x10) {
        uint64_t mag = bits == 32 ? static_cast<uint32_t>(v) : static_cast<uint64_t>(v);
        std::snprintf(buf, sizeof buf, upper ? "%llX" : "%llx", static_cast<unsigned long long>(mag));
        s = buf;
        if (showbase) s = (upper ? "0X" : "0x") + s;
    } else if ((flags & 0x38) == 0x20) {
        uint64_t mag = bits == 32 ? static_cast<uint32_t>(v) : static_cast<uint64_t>(v);
        std::snprintf(buf, sizeof buf, "%llo", static_cast<unsigned long long>(mag));
        s = buf;
        if (showbase && s != "0") s = "0" + s;
    } else {
        const bool negative = isSigned && v < 0;
        uint64_t mag = negative ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(mag));
        s = buf;
        if (negative) s = "-" + s;
        else if (isSigned && (flags & 0x800)) s = "+" + s;
    }
    return s;
}

// A double as STLport's __write_float: %f under fixed, %e under scientific, else %g with a
// precision of 6 unless one was set or showpoint is on (then at least 1); + and # as flagged.
static std::string floatText(double x, uint32_t flags, int precision) {
    const bool upper = (flags & 0x4000) != 0, showpos = (flags & 0x800) != 0, showpoint = (flags & 0x400) != 0;
    if (std::isnan(x) || std::isinf(x)) {
        std::string s = std::isnan(x) ? "nan" : x < 0 ? "-inf" : showpos ? "+inf" : "inf";
        if (upper) for (char &ch : s) ch = static_cast<char>(std::toupper(ch));
        return x != x && showpos ? "+" + s : s;
    }
    std::string fmt = "%";
    if (showpos) fmt += '+';
    if (showpoint) fmt += '#';
    fmt += ".*";
    const uint32_t field = flags & 0xC0;
    if (field == 0x40) fmt += upper ? 'F' : 'f';
    else if (field == 0x80) fmt += upper ? 'E' : 'e';
    else {
        if (showpoint || precision > 0) { if (precision == 0) precision = 1; } else precision = 6;
        fmt += upper ? 'G' : 'g';
    }
    if (precision < 0) precision = 6;
    std::vector<char> buf(static_cast<size_t>(precision) + 400);
    std::snprintf(buf.data(), buf.size(), fmt.c_str(), precision, x);
    return std::string(buf.data());
}

// The ostreambuf_iterator arrives in B5:B4 (streambuf, ok), the ios_base in A6, the fill in B6; the
// text is padded to the width - which this consumes - after a sign or a 0x under internal, and goes back as the iterator.
void Runtime::numPut(Cpu &c, const std::string &text, bool hexPrefixed) {
    const uint32_t buf = c.reg(Cpu::B4), ok = c.reg(Cpu::B4 + 1) & 0xff, ios = arg(c, 2);
    const uint32_t flags = c.load32(ios + 4);
    const int32_t width = static_cast<int32_t>(c.load32(ios + 28));
    c.store32(ios + 28, 0);
    const char fill = static_cast<char>(arg(c, 3));
    std::string s = text;
    if (width > static_cast<int32_t>(text.size())) {
        std::string pad(static_cast<size_t>(width) - text.size(), fill);
        const uint32_t dir = flags & 7;
        if (dir == 1) s = text + pad;
        else if (dir == 4 && !text.empty() && (text[0] == '+' || text[0] == '-')) s = text.substr(0, 1) + pad + text.substr(1);
        else if (dir == 4 && hexPrefixed && text.size() >= 2) s = text.substr(0, 2) + pad + text.substr(2);
        else s = pad + text;
    }
    if (ok) { if (FILE *f = hostOf(c, buf)) std::fwrite(s.data(), 1, s.size(), f); }
    c.setReg(Cpu::A4, buf);
    c.setReg(Cpu::A4 + 1, ok);
}
// With no reason given it ends as abort does, silently: what a terminate
// scope in the tables asks for, and what the hosts' pads do.
void Runtime::terminate(Cpu &cpu, const char *why) {
    std::fflush(stdout);
    if (why != nullptr) std::fprintf(stderr, "vm6747: terminate called: %s\n", why);
    cpu.exitWith(134);
    // exitWith stops the run; nothing after a terminate may continue.
    std::exit(134);
}

// ---- __dynamic_cast ------------------------------------------------------------
// The subobjects of the complete object, walked from its typeinfo: a class typeinfo is
// [vptr][name]; si adds [base]; vmi adds [flags][count] then [base][offset<<8 | flags] per base,
// a virtual base's offset naming the vbase_offset slot in the vtable of the subobject that holds it.
namespace {
struct Sub { uint32_t ti, addr; bool pub; };
void walk(Cpu &c, uint32_t ti, uint32_t addr, bool pub, std::vector<Sub> &out, int depth) {
    if (depth > 64) return;
    Sub s; s.ti = ti; s.addr = addr; s.pub = pub;
    out.push_back(s);
    uint32_t kind = c.load32(c.load32(ti));
    if (kind == 2) { walk(c, c.load32(ti + 8), addr, pub, out, depth + 1); return; }
    if (kind != 3) return;
    uint32_t count = c.load32(ti + 12);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bti = c.load32(ti + 16 + 8 * i);
        int32_t of = static_cast<int32_t>(c.load32(ti + 20 + 8 * i));
        int32_t off = of >> 8;
        bool basePub = (of & 2) != 0, virt = (of & 1) != 0;
        uint32_t baseAddr;
        if (virt) { uint32_t vptr = c.load32(addr); baseAddr = addr + static_cast<int32_t>(c.load32(vptr + off)); }
        else baseAddr = addr + off;
        walk(c, bti, baseAddr, pub && basePub, out, depth + 1);
    }
}
bool sameType(Cpu &c, uint32_t a, uint32_t b) {
    if (a == b) return true;
    return c.readString(c.load32(a + 4)) == c.readString(c.load32(b + 4));
}
// Is `target` a public base of the class `ti` describes - asked of the types
// alone, for a null pointer that has no object to walk.
bool publicBase(Cpu &c, uint32_t ti, uint32_t target, int depth) {
    if (depth > 64) return false;
    if (sameType(c, ti, target)) return true;
    uint32_t kind = c.load32(c.load32(ti));
    if (kind == 2) return publicBase(c, c.load32(ti + 8), target, depth + 1);
    if (kind != 3) return false;
    uint32_t count = c.load32(ti + 12);
    for (uint32_t i = 0; i < count; i++)
        if ((c.load32(ti + 20 + 8 * i) & 2) != 0 && publicBase(c, c.load32(ti + 16 + 8 * i), target, depth + 1))
            return true;
    return false;
}
}

uint32_t Runtime::dynamicCast(Cpu &c, uint32_t sub, uint32_t src, uint32_t dst) {
    if (sub == 0) return 0;
    uint32_t vptr = c.load32(sub);
    uint32_t complete = sub + static_cast<int32_t>(c.load32(vptr - 8));
    uint32_t cti = c.load32(vptr - 4);
    std::vector<Sub> subs;
    walk(c, cti, complete, true, subs, 0);
    // The source must be a public base of the complete object at `sub`.
    bool srcPublic = false;
    for (const Sub &s : subs) if (s.addr == sub && sameType(c, s.ti, src) && s.pub) srcPublic = true;
    if (!srcPublic) return 0;
    if (sameType(c, cti, dst)) return complete;
    // Otherwise the unique public dst subobject; two of them is ambiguous.
    uint32_t found = 0; int n = 0;
    for (const Sub &s : subs) if (s.pub && sameType(c, s.ti, dst)) { if (n == 0 || s.addr != found) { found = s.addr; n++; } }
    return n == 1 ? found : 0;
}

// ---- exceptions ------------------------------------------------------------------
// The tables are TI's (lib/src/tdeh_pr_common.cpp): the index gives a function's compact unwind
// word, or the address of its table - the word, then scope descriptors, then a zero. A descriptor
// is two halves, the range's length and its offset in the function (+2), whose low bits tell a cleanup (0) from a catch (2); then the pad, and a catch's type.

// A throw walks the frames twice, as TI's runtime does: phase one finds the barrier - the frame
// and catch descriptor whose type takes the exception, none terminating with nothing unwound -
// and phase two lands on every cleanup on the way and on the barrier's pad. README.md, "Exceptions", has the landing state.
void Runtime::loadExidx(Cpu &c) {
    if (ehLoaded_) return;
    ehLoaded_ = true;
    const std::vector<std::pair<uint32_t, uint32_t> > &index = c.program().exidxTables;
    for (size_t t = 0; t < index.size(); t++)
        for (uint32_t at = index[t].first; at + 8 <= index[t].first + index[t].second; at += 8) {
            ExidxEntry e; e.func = c.load32(at); e.word = c.load32(at + 4);
            exidx_.push_back(e);
        }
    std::sort(exidx_.begin(), exidx_.end(), [](const ExidxEntry &a, const ExidxEntry &b) { return a.func < b.func; });
}
const Runtime::ExidxEntry *Runtime::entryFor(uint32_t pc) const {
    const ExidxEntry *e = nullptr;
    for (const ExidxEntry &x : exidx_) { if (x.func > pc) break; e = &x; }
    return e;
}
// The frame above (pc, fp), read the way TI's unwinder would: the unwind word - inline, or the
// first of the table - compact form pr3 with SP restored from A15, then the saved registers a word
// each below A15 in the bitmask's order, A15 first and B3 after the B-file registers. False when pc has no entry, which ends the walk.
namespace { bool frameMask(Cpu &c, uint32_t entryWord, uint32_t &mask); bool wideTable(Cpu &c, uint32_t entryWord); }
bool Runtime::callerOf(Cpu &c, uint32_t pc, uint32_t fp, uint32_t &callerPc, uint32_t &callerFp) {
    const ExidxEntry *e = entryFor(pc);
    if (e == nullptr) return false;
    uint32_t mask = 0;
    if (!frameMask(c, e->word, mask)) return false;
    if ((mask & 1u << 12) == 0 || (mask & 1u << 5) == 0) return false;    // A15 and B3 saved
    int before = 0;
    for (int bit = 12; bit > 5; bit--) if (mask & 1u << bit) before++;   // A15, B15-B10
    callerFp = c.load32(fp);
    callerPc = c.load32(fp - 4 * static_cast<uint32_t>(before));
    return true;
}
// Where a function's descriptors begin, or 0 for an inline entry.
uint32_t Runtime::descriptors(Cpu &c_, const ExidxEntry &e) {
    return (e.word & 0x80000000u) != 0 ? 0 : e.word + 4 + (wideTable(c_, e.word) ? 4 * (c_.load32(e.word) >> 16 & 0xff) : 0);
}
Runtime::Exc *Runtime::excFor(uint32_t obj) {
    for (Exc &e : excs_) if (e.obj == obj) return &e;
    return nullptr;
}
// Does a handler of catchTi take an object of thrownTi? The same type, or a
// public base of it - found by walking the thrown object, which also gives
// the adjusted pointer a handler for the base receives.
bool Runtime::matches(Cpu &c, uint32_t obj, uint32_t thrownTi, uint32_t catchTi, uint32_t &adjusted) {
    if (catchTi == 0) { adjusted = obj; return true; }          // catch (...)
    // A pointer thrown - the type_info's vtable says so - is matched by [except.handle]/3: the
    // same pointee, a more qualified one, void, or a public base of a class pointee. What the
    // handler receives is the exception object's address, the adjusted pointer written into it -
    // TI's rts6740 does that where the Itanium runtimes return the value (C1, measured 2026-10-05).
    const uint32_t thrownKind = c.load32(c.load32(thrownTi)), catchKind = c.load32(c.load32(catchTi));
    if (thrownKind == 5 && catchKind == 5) {
        const uint32_t tFlags = c.load32(thrownTi + 8), cFlags = c.load32(catchTi + 8);
        if ((tFlags & ~cFlags) != 0) return false;             // const may be added, not dropped
        const uint32_t tPointee = c.load32(thrownTi + 12), cPointee = c.load32(catchTi + 12);
        const uint32_t value = c.load32(obj);
        adjusted = obj;
        if (c.readString(c.load32(cPointee + 4)) == "v") return true;
        if (sameType(c, tPointee, cPointee)) return true;
        // A pointer to a public base matches and is handed over UNADJUSTED - rts6740 does that
        // (C1b, measured 2026-10-05: cl6x 7.4.4's own build of `D : A, B` thrown as `D *`,
        // caught as `B *`, prints A's member on TI's C6747 simulator), so this does the same.
        if (value == 0) return publicBase(c, tPointee, cPointee, 0);
        std::vector<Sub> subs;
        walk(c, tPointee, value, true, subs, 0);
        for (const Sub &s : subs)
            if (s.pub && sameType(c, s.ti, cPointee)) return true;
        return false;
    }
    if (thrownKind == 5 || catchKind == 5) return false;
    std::vector<Sub> subs;
    walk(c, thrownTi, obj, true, subs, 0);
    for (const Sub &s : subs)
        if (s.pub && sameType(c, s.ti, catchTi)) { adjusted = s.addr; return true; }
    return false;
}
void Runtime::land(Cpu &c, uint32_t fp, uint32_t sp, uint32_t obj, uint32_t pad, bool withObject) {
    c.setReg(Cpu::A15, fp);
    c.setReg(Cpu::B15, sp);
    if (withObject) c.setReg(Cpu::A4, obj);
    else cleanupExc_ = obj;
    c.setReg(Cpu::B3, pad);
}
// One descriptor at `at`: its kind and range, the pad, a catch's type, and where the next one starts. False at the list's end.
namespace {
// Kind 0 a cleanup (the pad), 2 a catch (the pad, the type), 1 an
// exception specification (a count of allowed types, then those, then a
// pad when the count's top bit says so; this line writes a count of 0).
struct Descriptor { int kind; uint32_t begin, end, pad, rtti, count, next; };
bool readDescriptor(Cpu &c, uint32_t at, uint32_t func, Descriptor &d, bool wide) {
    if (at == 0 || c.load32(at) == 0) return false;
    const uint32_t w = wide ? 4 : 0;           // pr2: 32-bit length and offset
    uint32_t len = wide ? c.load32(at) : c.load16(at), off = wide ? c.load32(at + 4) : c.load16(at + 2);
    d.kind = static_cast<int>(((len & 1) << 1) | (off & 1));
    d.begin = func + (off & ~1u);
    d.end = d.begin + (len & ~1u);
    d.pad = c.load32(at + 4 + w);
    d.rtti = d.kind == 2 ? c.load32(at + 8 + w) : 0;
    d.count = d.kind == 1 ? c.load32(at + 4 + w) : 0;
    d.next = d.kind == 1 ? at + 8 + w + 4 * (d.count & 0x7fffffffu) + ((d.count & 0x80000000u) != 0 ? 4 : 0)
           : at + w + (d.kind == 2 ? 12 : 8);
    return true;
}
// The frame's pop mask: pr3's compact word, or pr2's byte-codes MV FP,SP; pop; RETURN.
bool frameMask(Cpu &c, uint32_t entryWord, uint32_t &mask) {
    const bool inl = (entryWord & 0x80000000u) != 0;
    const uint32_t word = inl ? entryWord : c.load32(entryWord);
    if ((word >> 24) == 0x83) {
        if ((word >> 17 & 0x7f) != 0x7f) return false;
        mask = word >> 4 & 0x1fff;
        return true;
    }
    if ((word >> 24) != 0x82 || inl) return false;
    std::vector<uint32_t> ops = { word >> 8 & 0xff, word & 0xff };
    for (uint32_t k = 1; k <= (word >> 16 & 0xff); k++)
        for (int s = 24; s >= 0; s -= 8) ops.push_back(c.load32(entryWord + 4 * k) >> s & 0xff);
    if (ops.size() < 4 || ops[0] != 0xd0 || (ops[1] & 0xe0) != 0x80 || ops[3] != 0xe7) return false;
    mask = (ops[1] & 0x1f) << 8 | ops[2];
    return true;
}
bool wideTable(Cpu &c, uint32_t entryWord) {
    return (entryWord & 0x80000000u) == 0 && (c.load32(entryWord) >> 24) == 0x82;
}
}
void Runtime::throwFrom(Cpu &c, uint32_t obj, uint32_t pc, uint32_t fp, uint32_t sp) {
    loadExidx(c);
    Exc *e = excFor(obj);
    if (e == nullptr) c.fault("__cxa_throw of something __cxa_allocate_exception did not give");
    // Phase one: the first catch descriptor in a frame's list whose range
    // holds the return address and whose type takes the exception.
    uint32_t p = pc, f = fp;
    e->barrierFp = 0;
    while (f != 0) {
        const ExidxEntry *x = entryFor(p);
        if (x == nullptr) break;
        Descriptor d;
        for (uint32_t at = descriptors(c, *x); readDescriptor(c, at, x->func, d, wideTable(c, x->word)); at = d.next) {
            if (p < d.begin || p >= d.end) continue;
            // An exception specification allowing nothing is a barrier too:
            // the cleanups below it run, then unexpected() ends the program.
            if (d.kind == 1 && (d.count & 0x7fffffffu) != 0) c.fault("an exception specification that allows types, which this line never writes");
            if (d.kind == 1) { e->barrierFp = f; e->barrierDesc = at; goto found; }
            if (d.kind != 2) continue;
            if (d.rtti == 0xfffffffeu) terminate(c, nullptr);      // the scope's word: end, as abort would
            uint32_t adj = obj;                                    // what catch (...) receives
            if (d.rtti == 0xffffffffu || matches(c, obj, e->ti, d.rtti, adj)) {
                e->barrierFp = f; e->barrierDesc = at; e->adjusted = adj;
                goto found;
            }
        }
        if (!callerOf(c, p, f, p, f)) break;
    }
    terminate(c, "an exception was not caught");
found:
    unwindTo(c, *e, pc, fp, sp, 0);
}
// Phase two, from the frame at (pc, fp, sp) and, when resuming after a cleanup, from the
// descriptor `from`: land on the first cleanup whose range holds pc, or on the barrier;
// otherwise on to the caller, whose SP is this frame's A15.
void Runtime::unwindTo(Cpu &c, Exc &e, uint32_t pc, uint32_t fp, uint32_t sp, uint32_t from) {
    uint32_t p = pc, f = fp, s = sp;
    while (f != 0) {
        const ExidxEntry *x = entryFor(p);
        if (x == nullptr) break;
        Descriptor d;
        for (uint32_t at = from != 0 ? from : descriptors(c, *x); readDescriptor(c, at, x->func, d, wideTable(c, x->word)); at = d.next) {
            if (d.kind == 0 && p >= d.begin && p < d.end) {
                e.cleanupPc = p; e.cleanupNext = d.next;
                land(c, f, s, e.obj, d.pad, false);
                return;
            }
            if (f == e.barrierFp && at == e.barrierDesc) {
                if (d.kind == 1) terminate(c, nullptr);                // the specification's unexpected()
                land(c, f, s, e.obj, d.pad, true);
                return;
            }
        }
        from = 0;
        if (f == e.barrierFp) c.fault("the handler frame's descriptors ran out before the barrier");
        s = f;
        popSaved(c, p, f);
        if (!callerOf(c, p, f, p, f)) break;
    }
    c.fault("the unwinder ran past the handler frame");
}
// Leaving the frame at (pc, fp) in phase two: its callee-saved registers come back off its save
// area, as TI's unwinder pops them into the register set it installs at the pad - a landing
// frame's own locals in A10-A13 and B10-B13 are what the frames below it saved.
void Runtime::popSaved(Cpu &c, uint32_t pc, uint32_t fp) {
    const ExidxEntry *e = entryFor(pc);
    if (e == nullptr) return;
    uint32_t mask = 0;
    if (!frameMask(c, e->word, mask)) return;
    static const int regOfBit[13] = { 10, 11, 12, 13, 14, 32 + 3, 32 + 10, 32 + 11, 32 + 12, 32 + 13, 32 + 14, 32 + 15, 15 };
    int at = 0;
    for (int bit = 12; bit >= 0; bit--) {
        if ((mask & 1u << bit) == 0) continue;
        const int reg = regOfBit[bit];
        if (reg != 15 && reg != 32 + 3 && reg != 32 + 15) c.setReg(reg, c.load32(fp - 4 * static_cast<uint32_t>(at)));
        at++;
    }
}

// ---- the library -----------------------------------------------------------------
std::vector<std::string> Runtime::names() {
    static const char *const k[] = {
        "putchar", "puts", "printf", "vprintf", "sprintf", "vsprintf", "snprintf", "vsnprintf",
        "fprintf", "vfprintf", "fputs", "fputc", "putc", "fwrite", "fflush", "fopen", "fclose",
        "fgetc", "getc", "getchar", "fgets", "fread", "ftell", "fseek", "rewind", "feof", "remove",
        "perror", "ferror", "clearerr", "__errno_location", "__c6xabi_errno_addr", "__assert_fail", "__assert_rtn", "_assert", "__c6xabi_abort_msg",
        "sscanf", "fscanf", "scanf", "ungetc", "gets",
        "exit", "abort", "atexit", "close", "malloc", "calloc", "realloc", "free",
        "memcpy", "memmove", "memset", "memcmp", "memchr",
        "strlen", "strcpy", "strncpy", "strcat", "strncat", "strcmp", "strncmp", "strchr", "strrchr",
        "strstr", "strpbrk", "strspn", "strcspn", "strtok", "strerror", "strdup",
        "atoi", "atol", "atof", "strtol", "strtoul", "strtod", "abs", "labs", "llabs", "rand", "srand",
        "qsort", "bsearch",
        "isalnum", "isalpha", "iscntrl", "isdigit", "isgraph", "islower", "isprint", "ispunct",
        "isspace", "isupper", "isxdigit", "tolower", "toupper",
        "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
        "exp", "log", "log10", "pow", "fabs", "floor", "ceil", "fmod", "ldexp", "frexp", "modf", "trunc", "round", "__c6xabi_trunc", "__c6xabi_nround",
        "log2", "cbrt", "hypot", "exp2", "log1p", "expm1",
        "sqrtf", "fabsf", "floorf", "ceilf",
        "setjmp", "_setjmp", "longjmp", "time", "clock",
        "signal", "raise", "mktime", "localtime", "gmtime", "strftime", "difftime",
        "setlocale", "localeconv",
        // the C++ ABI
        "_Znwm", "_Znam", "_Znwj", "_Znaj", "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm", "_ZdlPvj", "_ZdaPvj",
        "__cxa_guard_acquire", "__cxa_guard_release", "__cxa_guard_abort", "__cxa_atexit",
        "__dynamic_cast", "__cxa_pure_virtual", "__cxa_deleted_virtual",
        "__cxa_allocate_exception", "__cxa_free_exception", "__cxa_throw", "__cxa_rethrow",
        "__cxa_begin_catch", "__cxa_end_catch", "__cxa_get_exception_ptr", "_Unwind_Resume", "__cxa_end_cleanup",
        "__gxx_personality_v0", "__cxa_bad_cast", "__cxa_bad_typeid", "_ZSt9terminatev",
        "_ZNKSt9type_infoeqERKS_", "_ZNKSt9type_infoneERKS_", "_ZNKSt9type_info4nameEv",
        "_ZNKSt9type_info6beforeERKS_",
        "__cxa_vec_new", "__cxa_vec_new2", "__cxa_vec_new3", "__cxa_vec_ctor", "__cxa_vec_cctor",
        "__cxa_vec_dtor", "__cxa_vec_cleanup", "__cxa_vec_delete", "__cxa_vec_delete2", "__cxa_vec_delete3",
        // cl6x's <iostream>: what its compiled runtime holds and the program's inlined STLport calls
        "_ZNSt8ios_base4InitC1Ev", "_ZNSt8ios_base4InitC2Ev", "_ZNSt8ios_base4InitD1Ev", "_ZNSt8ios_base4InitD2Ev",
        "_ZNSt6localeC1ERKS_", "_ZNSt6localeC2ERKS_", "_ZNSt6localeD1Ev", "_ZNSt6localeD2Ev",
        "_ZNKSt6locale12_M_use_facetERKNS_2idE", "_ZNKSt6locale11_M_get_facetERKNS_2idE",
        "_ZNSt4priv11_GetFacetIdEPKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE",
        "_ZNSt8ios_base16_M_throw_failureEv",
        "__vm6747_sb_sync", "__vm6747_sb_xsputn", "__vm6747_sb_xsputnc", "__vm6747_sb_overflow",
        "__vm6747_num_put_bool", "__vm6747_num_put_long", "__vm6747_num_put_ulong", "__vm6747_num_put_double",
        "__vm6747_num_put_ldouble", "__vm6747_num_put_llong", "__vm6747_num_put_ullong", "__vm6747_num_put_pointer",
        "__c6xabi_divi", "__c6xabi_divu", "__c6xabi_remi", "__c6xabi_remu",
        "__c6xabi_divlli", "__c6xabi_divull", "__c6xabi_remlli", "__c6xabi_remull",
        "__c6xabi_divf", "__c6xabi_divd", "__c6xabi_fixfu", "__c6xabi_fixdu",
        "__c6xabi_fixflli", "__c6xabi_fixdlli", "__c6xabi_fixfull", "__c6xabi_fixdull",
        "__c6xabi_fltllif", "__c6xabi_fltllid", "__c6xabi_fltullf", "__c6xabi_fltulld",
    };
    return std::vector<std::string>(k, k + sizeof k / sizeof k[0]);
}

bool Runtime::call(const std::string &n, Cpu &c) {
    // ---- output ----
    if (n == "putchar") { if (host(1)) std::putchar(static_cast<int>(arg(c, 0))); ret(c, arg(c, 0) & 0xff); return true; }
    if (n == "puts") { if (host(1)) { std::fputs(c.readString(arg(c, 0)).c_str(), stdout); std::putchar('\n'); } ret(c, 1); return true; }
    if (n == "close") { uint32_t fd = arg(c, 0); if (fd <= 2) closed_[fd] = true; ret(c, fd <= 2 ? 0 : static_cast<uint32_t>(-1)); return true; }
    if (n == "printf") {
        Args a = { c, variadicAt(c, 1) };
        std::string fmt = c.readString(a.word());
        std::string s = format(c, fmt, a);
        if (host(1)) std::fwrite(s.data(), 1, s.size(), stdout);
        ret(c, static_cast<uint32_t>(s.size()));
        return true;
    }
    if (n == "vprintf") {
        Args a = { c, arg(c, 1) };
        std::string s = format(c, c.readString(arg(c, 0)), a);
        if (host(1)) std::fwrite(s.data(), 1, s.size(), stdout);
        ret(c, static_cast<uint32_t>(s.size()));
        return true;
    }
    if (n == "sprintf" || n == "snprintf" || n == "vsprintf" || n == "vsnprintf") {
        uint32_t buf = arg(c, 0);
        bool sized = n == "snprintf" || n == "vsnprintf";
        uint32_t cap = sized ? arg(c, 1) : 0xffffffffu;
        int fmtIx = sized ? 2 : 1;
        Args a = n[0] == 'v' ? Args{ c, arg(c, fmtIx + 1) } : Args{ c, variadicAt(c, fmtIx + 1) };
        std::string fmt = c.readString(n[0] == 'v' ? arg(c, fmtIx) : a.word());
        std::string s = format(c, fmt, a);
        if (cap > 0) {
            uint32_t k = static_cast<uint32_t>(s.size()) < cap - 1 ? static_cast<uint32_t>(s.size()) : cap - 1;
            c.writeBytes(buf, s.data(), k);
            c.store8(buf + k, 0);
        }
        ret(c, static_cast<uint32_t>(s.size()));
        return true;
    }
    if (n == "fprintf" || n == "vfprintf") {
        uint32_t stream = streamNumber(c, arg(c, 0));
        Args a = n[0] == 'v' ? Args{ c, arg(c, 2) } : Args{ c, variadicAt(c, 2) };
        std::string fmt = c.readString(n[0] == 'v' ? arg(c, 1) : a.word());
        std::string s = format(c, fmt, a);
        FILE *f = stream == 2 || stream == 3 ? host(static_cast<int>(stream) - 1) : nullptr;
        if (stream >= 4 && stream - 4 < files_.size()) { files_[stream - 4].data += s; }
        else if (f != nullptr) std::fwrite(s.data(), 1, s.size(), f);
        ret(c, static_cast<uint32_t>(s.size()));
        return true;
    }
    if (n == "fputs" || n == "fputc" || n == "putc" || n == "fwrite") {
        uint32_t stream = streamNumber(c, n == "fwrite" ? arg(c, 3) : arg(c, 1));
        std::string s;
        if (n == "fputs") s = c.readString(arg(c, 0));
        else if (n == "fwrite") { uint32_t k = arg(c, 1) * arg(c, 2); for (uint32_t i = 0; i < k; i++) s += static_cast<char>(c.load8(arg(c, 0) + i)); }
        else s = std::string(1, static_cast<char>(arg(c, 0)));
        if (stream == 2 || stream == 3) { if (host(static_cast<int>(stream) - 1)) std::fwrite(s.data(), 1, s.size(), host(static_cast<int>(stream) - 1)); }
        else if (stream >= 4 && stream - 4 < files_.size()) files_[stream - 4].data += s;
        ret(c, n == "fwrite" ? arg(c, 2) : n == "fputs" ? 1 : (arg(c, 0) & 0xff));
        return true;
    }
    if (n == "fflush") { std::fflush(stdout); ret(c, 0); return true; }
    if (n == "sscanf" || n == "fscanf" || n == "scanf") {
        // sscanf(str, fmt, ...) and fscanf(stream, fmt, ...): the first in A4, the format and the
        // pointers on the stack; scanf(fmt, ...) is fscanf on stdin, its format the stack's first word.
        // Parsed over a string - a stream's rest, which then advances by what was consumed.
        File *fs = n == "fscanf" ? streamFile(streamNumber(c, arg(c, 0))) : n == "scanf" ? streamFile(1) : nullptr;
        if (n != "sscanf" && fs == nullptr) { ret(c, 0xffffffffu); return true; }
        std::string in = fs != nullptr ? fs->data.substr(fs->pos) : c.readString(arg(c, 0));
        Args a = { c, variadicAt(c, n == "scanf" ? 1 : 2) };
        std::string fmt = c.readString(a.word());
        size_t i = 0;
        int assigned = 0;
        auto skipSpace = [&]() { while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i]))) i++; };
        for (size_t k = 0; k < fmt.size(); k++) {
            char f = fmt[k];
            if (std::isspace(static_cast<unsigned char>(f))) { skipSpace(); continue; }
            if (f != '%') { if (i < in.size() && in[i] == f) i++; else break; continue; }
            k++;
            bool suppress = false;
            if (k < fmt.size() && fmt[k] == '*') { suppress = true; k++; }
            int width = 0;
            while (k < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[k]))) width = width * 10 + (fmt[k++] - '0');
            int longs = 0; bool shortInt = false;
            while (k < fmt.size() && std::strchr("hlLjzt", fmt[k])) { if (fmt[k] == 'l' || fmt[k] == 'j') longs++; if (fmt[k] == 'L') longs = 2; if (fmt[k] == 'h') shortInt = true; k++; }
            if (k >= fmt.size()) break;
            char conv = fmt[k];
            if (conv == 'n') { if (!suppress) c.store32(a.word(), static_cast<uint32_t>(i)); continue; }
            if (conv == '%') { if (i < in.size() && in[i] == '%') i++; else break; continue; }
            if (conv != 'c') skipSpace();
            if (i >= in.size()) break;
            std::string field = width > 0 ? in.substr(i, static_cast<size_t>(width)) : in.substr(i);
            const char *start = field.c_str();
            char *end = nullptr;
            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
                long long v = conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o'
                            ? static_cast<long long>(std::strtoull(start, &end, base)) : std::strtoll(start, &end, base);
                if (end == start) break;
                i += static_cast<size_t>(end - start);
                if (suppress) continue;
                uint32_t p = a.word();
                if (longs >= 2) c.store64(p, static_cast<uint64_t>(v));
                else if (shortInt) c.store16(p, static_cast<uint16_t>(v));
                else c.store32(p, static_cast<uint32_t>(v));
                assigned++;
            } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'a') {
                double v = std::strtod(start, &end);
                if (end == start) break;
                i += static_cast<size_t>(end - start);
                if (suppress) continue;
                uint32_t p = a.word();
                if (longs >= 1) { uint64_t b; std::memcpy(&b, &v, 8); c.store64(p, b); }
                else { float fv = static_cast<float>(v); uint32_t b; std::memcpy(&b, &fv, 4); c.store32(p, b); }
                assigned++;
            } else if (conv == 's') {
                size_t j = 0;
                while (j < field.size() && !std::isspace(static_cast<unsigned char>(field[j]))) j++;
                if (j == 0) break;
                i += j;
                if (suppress) continue;
                uint32_t p = a.word();
                c.writeBytes(p, field.data(), static_cast<uint32_t>(j)); c.store8(p + static_cast<uint32_t>(j), 0);
                assigned++;
            } else if (conv == 'c') {
                size_t cnt = width > 0 ? static_cast<size_t>(width) : 1;
                if (i + cnt > in.size()) break;
                if (!suppress) { uint32_t p = a.word(); c.writeBytes(p, in.data() + i, static_cast<uint32_t>(cnt)); assigned++; }
                i += cnt;
            } else break;
        }
        if (fs != nullptr) { fs->pos += i; if (assigned == 0 && i == 0 && fs->pos >= fs->data.size()) { ret(c, 0xffffffffu); return true; } }
        ret(c, static_cast<uint32_t>(assigned));
        return true;
    }
    if (n == "ungetc") {
        File *fs = streamFile(streamNumber(c, arg(c, 1)));
        uint32_t ch = arg(c, 0) & 0xff;
        if (fs == nullptr || ch == 0xff) { ret(c, 0xffffffffu); return true; }
        if (fs->pos > 0 && static_cast<unsigned char>(fs->data[fs->pos - 1]) == ch) fs->pos--;
        else fs->data.insert(fs->pos, 1, static_cast<char>(ch));
        ret(c, ch);
        return true;
    }
    if (n == "ferror" || n == "clearerr") { ret(c, 0); return true; }
    if (n == "__assert_fail" || n == "__assert_rtn" || n == "_assert") {
        // glibc's (expr, file, line, function), Darwin's (function, file, line, expr), UCRT's (expr, file, line)
        std::string expr = c.readString(arg(c, n == "__assert_rtn" ? 3 : 0)), file = c.readString(arg(c, 1));
        std::fflush(stdout);
        std::fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", expr.c_str(), file.c_str(), arg(c, 2));
        c.exitWith(134);
        return true;
    }
    if (n == "__errno_location" || n == "__c6xabi_errno_addr") { ret(c, errnoAt(c)); return true; }
    if (n == "__c6xabi_abort_msg") {                // TI's assert: the whole message, built by the macro
        std::fflush(stdout);
        std::fputs(c.readString(arg(c, 0)).c_str(), stderr);
        c.exitWith(134);
        return true;
    }
    if (n == "perror") { std::string s = c.readString(arg(c, 0)); std::fprintf(stderr, "%s: error\n", s.c_str()); return true; }
    // ---- files: kept in memory on the host until closed ----
    if (n == "fopen") {
        std::string path = c.readString(arg(c, 0)), mode = c.readString(arg(c, 1));
#ifdef _WIN32
        // A program that writes to /tmp means the scratch directory; give it
        // this host's, so the same corpus runs here.
        if (path.compare(0, 5, "/tmp/") == 0) {
            const char *t = std::getenv("TEMP");
            path = std::string(t != nullptr ? t : ".") + "\\" + path.substr(5);
        }
#endif
        File f; f.path = path; f.pos = 0; f.write = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos;
        if (!f.write || mode.find('+') != std::string::npos || mode[0] == 'a') {
            FILE *h = std::fopen(path.c_str(), "rb");
            if (h == nullptr && !f.write) { setErrno(c, 2); ret(c, 0); return true; }   // ENOENT
            if (h != nullptr) { char b[4096]; size_t k; while ((k = std::fread(b, 1, sizeof b, h)) > 0) f.data.append(b, k); std::fclose(h); }
            if (mode[0] == 'w') f.data.clear();
            if (mode[0] == 'a') f.pos = f.data.size();
        }
        files_.push_back(f);
        ret(c, 4 + static_cast<uint32_t>(files_.size()) - 1);
        return true;
    }
    if (n == "fclose") {
        uint32_t s = arg(c, 0);
        if (s >= 4 && s - 4 < files_.size()) {
            File &f = files_[s - 4];
            if (f.write) { FILE *h = std::fopen(f.path.c_str(), "wb"); if (h) { std::fwrite(f.data.data(), 1, f.data.size(), h); std::fclose(h); } }
        }
        ret(c, 0);
        return true;
    }
    if (n == "fgetc" || n == "getc" || n == "getchar") {
        File *f = streamFile(n == "getchar" ? 1 : streamNumber(c, arg(c, 0)));
        int ch = -1;
        if (f != nullptr && f->pos < f->data.size()) ch = static_cast<unsigned char>(f->data[f->pos++]);
        ret(c, static_cast<uint32_t>(ch));
        return true;
    }
    if (n == "gets") {
        // C90's gets: a line from stdin with no bound, the newline read and not stored.
        uint32_t buf = arg(c, 0);
        File *f = streamFile(1);
        if (f == nullptr || f->pos >= f->data.size()) { ret(c, 0); return true; }
        std::string line;
        while (f->pos < f->data.size()) { char ch = f->data[f->pos++]; if (ch == '\n') break; line += ch; }
        c.writeBytes(buf, line.data(), static_cast<uint32_t>(line.size()));
        c.store8(buf + static_cast<uint32_t>(line.size()), 0);
        ret(c, buf);
        return true;
    }
    if (n == "fgets") {
        uint32_t buf = arg(c, 0), cap = arg(c, 1), s = streamNumber(c, arg(c, 2));
        std::string line;
        bool any = false;
        File *f = streamFile(s);
        while (line.size() + 1 < cap) {
            int ch = -1;
            if (f != nullptr && f->pos < f->data.size()) ch = static_cast<unsigned char>(f->data[f->pos++]);
            if (ch < 0) break;
            any = true; line += static_cast<char>(ch);
            if (ch == '\n') break;
        }
        if (!any) { ret(c, 0); return true; }
        c.writeBytes(buf, line.data(), static_cast<uint32_t>(line.size()));
        c.store8(buf + static_cast<uint32_t>(line.size()), 0);
        ret(c, buf);
        return true;
    }
    if (n == "fread") {
        uint32_t buf = arg(c, 0), k = arg(c, 1) * arg(c, 2), s = arg(c, 3);
        uint32_t got = 0;
        if (s >= 4 && s - 4 < files_.size()) { File &f = files_[s - 4]; while (got < k && f.pos < f.data.size()) c.store8(buf + got++, static_cast<uint8_t>(f.data[f.pos++])); }
        ret(c, arg(c, 1) ? got / arg(c, 1) : 0);
        return true;
    }
    if (n == "ftell") { uint32_t s = arg(c, 0); ret(c, s >= 4 && s - 4 < files_.size() ? static_cast<uint32_t>(files_[s - 4].write ? files_[s - 4].data.size() : files_[s - 4].pos) : 0); return true; }
    if (n == "fseek") {
        uint32_t s = arg(c, 0); int32_t off = static_cast<int32_t>(arg(c, 1)); uint32_t whence = arg(c, 2);
        if (s >= 4 && s - 4 < files_.size()) { File &f = files_[s - 4]; size_t base = whence == 0 ? 0 : whence == 1 ? f.pos : f.data.size(); f.pos = base + off; }
        ret(c, 0);
        return true;
    }
    if (n == "rewind") { uint32_t s = arg(c, 0); if (s >= 4 && s - 4 < files_.size()) files_[s - 4].pos = 0; return true; }
    if (n == "feof") { File *f = streamFile(streamNumber(c, arg(c, 0))); ret(c, f != nullptr && f->pos >= f->data.size()); return true; }
    if (n == "remove") { ret(c, std::remove(c.readString(arg(c, 0)).c_str()) == 0 ? 0 : 0xffffffffu); return true; }
    // ---- process ----
    if (n == "exit") { std::fflush(stdout); c.exitWith(static_cast<int>(arg(c, 0))); return true; }
    if (n == "abort") { std::fflush(stdout); c.exitWith(134); return true; }   // silently, as the real one
    if (n == "atexit") { AtExit a; a.fn = arg(c, 0); a.arg = 0; atExit_.push_back(a); ret(c, 0); return true; }
    if (n == "time") { uint32_t p = arg(c, 0); uint32_t t = static_cast<uint32_t>(std::time(nullptr)); if (p) c.store32(p, t); ret(c, t); return true; }
    // ---- signals: a table of handlers, raise calls one ----
    if (n == "signal") {
        uint32_t sig = arg(c, 0), handler = arg(c, 1);
        if (sig >= 32) { ret(c, 0xffffffffu); return true; }
        uint32_t prev = handlers_[sig];
        handlers_[sig] = handler;
        ret(c, prev);
        return true;
    }
    if (n == "raise") {
        uint32_t sig = arg(c, 0);
        uint32_t h = sig < 32 ? handlers_[sig] : 0;
        if (h == 1) { ret(c, 0); return true; }                       // SIG_IGN
        if (h == 0) { std::fflush(stdout); std::fprintf(stderr, "vm6747: signal %u\n", sig); c.exitWith(128 + static_cast<int>(sig)); return true; }
        handlers_[sig] = 0;                                           // reset, as SIGTERM's default does
        c.callback(h, sig, 0);
        ret(c, 0);
        return true;
    }
    // ---- time: struct tm is nine ints, then a long and a pointer here ----
    if (n == "mktime" || n == "strftime" || n == "localtime" || n == "gmtime" || n == "difftime") {
        if (n == "difftime") { retDouble(c, static_cast<double>(static_cast<int32_t>(arg(c, 0))) - static_cast<double>(static_cast<int32_t>(arg(c, 1)))); return true; }
        auto readTm = [&](uint32_t at, std::tm &t) {
            std::memset(&t, 0, sizeof t);
            int *f[9] = { &t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst };
            for (int i = 0; i < 9; i++) *f[i] = static_cast<int32_t>(c.load32(at + 4 * i));
        };
        auto writeTm = [&](uint32_t at, const std::tm &t) {
            const int v[9] = { t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst };
            for (int i = 0; i < 9; i++) c.store32(at + 4 * i, static_cast<uint32_t>(v[i]));
            c.store32(at + 36, 0); c.store32(at + 40, 0);
        };
        if (n == "mktime") { std::tm t; readTm(arg(c, 0), t); std::time_t r = std::mktime(&t); writeTm(arg(c, 0), t); ret(c, static_cast<uint32_t>(r)); return true; }
        if (n == "strftime") {
            std::tm t; readTm(arg(c, 3), t);
            std::string fmt = c.readString(arg(c, 2));
            char buf[512];
            size_t k = std::strftime(buf, sizeof buf, fmt.c_str(), &t);
            if (k + 1 > arg(c, 1)) { ret(c, 0); return true; }
            c.writeBytes(arg(c, 0), buf, static_cast<uint32_t>(k) + 1);
            ret(c, static_cast<uint32_t>(k));
            return true;
        }
        static uint32_t tmAt = 0;
        if (tmAt == 0) tmAt = allocate(c, 44);
        std::time_t tt = static_cast<int32_t>(c.load32(arg(c, 0)));
        std::tm *t = n == "gmtime" ? std::gmtime(&tt) : std::localtime(&tt);
        if (t == nullptr) { ret(c, 0); return true; }
        writeTm(tmAt, *t);
        ret(c, tmAt);
        return true;
    }
    // ---- locale: "C", and nothing else ----
    if (n == "setlocale") {
        static uint32_t name = 0;
        if (name == 0) { name = allocate(c, 8); c.writeBytes(name, "C\0", 2); }
        uint32_t want = arg(c, 1);
        if (want != 0) { std::string s = c.readString(want); if (!s.empty() && s != "C" && s != "POSIX") { ret(c, 0); return true; } }
        ret(c, name);
        return true;
    }
    if (n == "localeconv") {
        // Ten char * then eight char, as <locale.h> lays struct lconv out.
        static uint32_t lc = 0;
        if (lc == 0) {
            lc = allocate(c, 48);
            uint32_t dot = allocate(c, 8); c.writeBytes(dot, ".\0", 2);
            uint32_t empty = allocate(c, 8); c.store8(empty, 0);
            c.store32(lc, dot);
            for (int i = 1; i < 10; i++) c.store32(lc + 4 * i, empty);
            for (int i = 0; i < 8; i++) c.store8(lc + 40 + i, 127);   // CHAR_MAX: not available
        }
        ret(c, lc);
        return true;
    }
    if (n == "clock") { ret(c, static_cast<uint32_t>(c.cycle() / 1000)); return true; }
    // ---- memory ----
    if (n == "malloc") { ret(c, allocate(c, arg(c, 0))); return true; }
    if (n == "calloc") { uint32_t k = arg(c, 0) * arg(c, 1); uint32_t p = allocate(c, k); for (uint32_t i = 0; i < k; i++) c.store8(p + i, 0); ret(c, p); return true; }
    if (n == "realloc") {
        uint32_t old = arg(c, 0), size = arg(c, 1);
        uint32_t p = allocate(c, size);
        if (old != 0) {
            uint32_t oldSize = 0;
            for (const Block &b : blocks_) if (b.at == old) oldSize = b.size;
            for (uint32_t i = 0; i < oldSize && i < size; i++) c.store8(p + i, c.load8(old + i));
            release(c, old);
        }
        ret(c, p);
        return true;
    }
    if (n == "free") { release(c, arg(c, 0)); return true; }
    if (n == "memcpy" || n == "memmove") {
        uint32_t d = arg(c, 0), s = arg(c, 1), k = arg(c, 2);
        std::string tmp; for (uint32_t i = 0; i < k; i++) tmp += static_cast<char>(c.load8(s + i));
        for (uint32_t i = 0; i < k; i++) c.store8(d + i, static_cast<uint8_t>(tmp[i]));
        ret(c, d); return true;
    }
    if (n == "memset") { uint32_t d = arg(c, 0); for (uint32_t i = 0; i < arg(c, 2); i++) c.store8(d + i, static_cast<uint8_t>(arg(c, 1))); ret(c, d); return true; }
    if (n == "memcmp") {
        uint32_t a = arg(c, 0), b = arg(c, 1), k = arg(c, 2);
        for (uint32_t i = 0; i < k; i++) { int x = c.load8(a + i), y = c.load8(b + i); if (x != y) { ret(c, static_cast<uint32_t>(x - y)); return true; } }
        ret(c, 0); return true;
    }
    if (n == "memchr") { uint32_t p = arg(c, 0); for (uint32_t i = 0; i < arg(c, 2); i++) if (c.load8(p + i) == (arg(c, 1) & 0xff)) { ret(c, p + i); return true; } ret(c, 0); return true; }
    // ---- strings ----
    if (n == "strlen") { ret(c, static_cast<uint32_t>(c.readString(arg(c, 0)).size())); return true; }
    if (n == "strcpy" || n == "strcat") {
        uint32_t d = arg(c, 0); std::string s = c.readString(arg(c, 1));
        uint32_t at = d + (n == "strcat" ? static_cast<uint32_t>(c.readString(d).size()) : 0);
        c.writeBytes(at, s.data(), static_cast<uint32_t>(s.size())); c.store8(at + static_cast<uint32_t>(s.size()), 0);
        ret(c, d); return true;
    }
    if (n == "strncpy") {
        uint32_t d = arg(c, 0); std::string s = c.readString(arg(c, 1)); uint32_t k = arg(c, 2);
        for (uint32_t i = 0; i < k; i++) c.store8(d + i, i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
        ret(c, d); return true;
    }
    if (n == "strncat") {
        uint32_t d = arg(c, 0); std::string s = c.readString(arg(c, 1)); uint32_t k = arg(c, 2);
        if (s.size() > k) s.resize(k);
        uint32_t at = d + static_cast<uint32_t>(c.readString(d).size());
        c.writeBytes(at, s.data(), static_cast<uint32_t>(s.size())); c.store8(at + static_cast<uint32_t>(s.size()), 0);
        ret(c, d); return true;
    }
    if (n == "strcmp") { int r = std::strcmp(c.readString(arg(c, 0)).c_str(), c.readString(arg(c, 1)).c_str()); ret(c, static_cast<uint32_t>(r < 0 ? -1 : r > 0 ? 1 : 0)); return true; }
    if (n == "strncmp") { int r = std::strncmp(c.readString(arg(c, 0)).c_str(), c.readString(arg(c, 1)).c_str(), arg(c, 2)); ret(c, static_cast<uint32_t>(r < 0 ? -1 : r > 0 ? 1 : 0)); return true; }
    if (n == "strchr" || n == "strrchr") {
        uint32_t p = arg(c, 0); std::string s = c.readString(p); char ch = static_cast<char>(arg(c, 1));
        size_t k = n == "strchr" ? s.find(ch) : s.rfind(ch);
        if (ch == 0) k = s.size();
        ret(c, k == std::string::npos ? 0 : p + static_cast<uint32_t>(k)); return true;
    }
    if (n == "strstr") { uint32_t p = arg(c, 0); std::string s = c.readString(p); size_t k = s.find(c.readString(arg(c, 1))); ret(c, k == std::string::npos ? 0 : p + static_cast<uint32_t>(k)); return true; }
    if (n == "strpbrk") { uint32_t p = arg(c, 0); std::string s = c.readString(p); size_t k = s.find_first_of(c.readString(arg(c, 1))); ret(c, k == std::string::npos ? 0 : p + static_cast<uint32_t>(k)); return true; }
    if (n == "strspn") { ret(c, static_cast<uint32_t>(std::strspn(c.readString(arg(c, 0)).c_str(), c.readString(arg(c, 1)).c_str()))); return true; }
    if (n == "strcspn") { ret(c, static_cast<uint32_t>(std::strcspn(c.readString(arg(c, 0)).c_str(), c.readString(arg(c, 1)).c_str()))); return true; }
    if (n == "strtok") {
        static uint32_t saved = 0;
        uint32_t p = arg(c, 0) ? arg(c, 0) : saved;
        std::string delim = c.readString(arg(c, 1));
        if (p == 0) { ret(c, 0); return true; }
        while (c.load8(p) != 0 && delim.find(static_cast<char>(c.load8(p))) != std::string::npos) p++;
        if (c.load8(p) == 0) { saved = 0; ret(c, 0); return true; }
        uint32_t start = p;
        while (c.load8(p) != 0 && delim.find(static_cast<char>(c.load8(p))) == std::string::npos) p++;
        if (c.load8(p) != 0) { c.store8(p, 0); saved = p + 1; } else saved = 0;
        ret(c, start); return true;
    }
    if (n == "strerror") { static uint32_t msg = 0; if (msg == 0) { msg = allocate(c, 32); c.writeBytes(msg, "error\0", 6); } ret(c, msg); return true; }
    if (n == "strdup") { std::string s = c.readString(arg(c, 0)); uint32_t p = allocate(c, static_cast<uint32_t>(s.size()) + 1); c.writeBytes(p, s.c_str(), static_cast<uint32_t>(s.size()) + 1); ret(c, p); return true; }
    // ---- conversions ----
    if (n == "atoi" || n == "atol") { ret(c, static_cast<uint32_t>(std::atoi(c.readString(arg(c, 0)).c_str()))); return true; }
    if (n == "atof") { retDouble(c, std::atof(c.readString(arg(c, 0)).c_str())); return true; }
    if (n == "strtol" || n == "strtoul" || n == "strtod") {
        uint32_t p = arg(c, 0); std::string s = c.readString(p); uint32_t endp = arg(c, 1);
        char *end = nullptr;
        if (n == "strtod") retDouble(c, std::strtod(s.c_str(), &end));
        else if (n == "strtol") ret(c, static_cast<uint32_t>(std::strtol(s.c_str(), &end, static_cast<int>(arg(c, 2)))));
        else ret(c, static_cast<uint32_t>(std::strtoul(s.c_str(), &end, static_cast<int>(arg(c, 2)))));
        if (endp != 0) c.store32(endp, p + static_cast<uint32_t>(end - s.c_str()));
        return true;
    }
    if (n == "abs" || n == "labs") { int32_t v = static_cast<int32_t>(arg(c, 0)); ret(c, static_cast<uint32_t>(v < 0 ? -v : v)); return true; }
    if (n == "llabs") { int64_t v = static_cast<int64_t>(argWide(c, 0)); retWide(c, static_cast<uint64_t>(v < 0 ? -v : v)); return true; }
    if (n == "rand") { ret(c, static_cast<uint32_t>(std::rand())); return true; }
    if (n == "srand") { std::srand(arg(c, 0)); return true; }
    if (n == "qsort") {
        uint32_t base = arg(c, 0), count = arg(c, 1), size = arg(c, 2), cmp = arg(c, 3);
        // An insertion sort through the program's comparator: the order of
        // comparisons differs from the host's qsort, which a comparator with
        // side effects would show, and nothing else.
        std::string tmp(size, '\0');
        for (uint32_t i = 1; i < count; i++) {
            for (uint32_t j = i; j > 0; j--) {
                uint32_t a = base + (j - 1) * size, b = base + j * size;
                if (static_cast<int32_t>(c.callback(cmp, a, b)) <= 0) break;
                for (uint32_t k = 0; k < size; k++) { uint8_t x = c.load8(a + k); c.store8(a + k, c.load8(b + k)); c.store8(b + k, x); }
            }
        }
        return true;
    }
    if (n == "bsearch") {
        uint32_t key = arg(c, 0), base = arg(c, 1), count = arg(c, 2), size = arg(c, 3), cmp = arg(c, 4);
        uint32_t lo = 0, hi = count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int32_t r = static_cast<int32_t>(c.callback(cmp, key, base + mid * size));
            if (r == 0) { ret(c, base + mid * size); return true; }
            if (r < 0) hi = mid; else lo = mid + 1;
        }
        ret(c, 0); return true;
    }
    // ---- ctype ----
    {
        static const char *const ct[] = { "isalnum", "isalpha", "iscntrl", "isdigit", "isgraph", "islower", "isprint", "ispunct", "isspace", "isupper", "isxdigit" };
        for (const char *name : ct) if (n == name) {
            int ch = static_cast<int>(arg(c, 0)); int r;
            if (n == "isalnum") r = std::isalnum(ch); else if (n == "isalpha") r = std::isalpha(ch); else if (n == "iscntrl") r = std::iscntrl(ch);
            else if (n == "isdigit") r = std::isdigit(ch); else if (n == "isgraph") r = std::isgraph(ch); else if (n == "islower") r = std::islower(ch);
            else if (n == "isprint") r = std::isprint(ch); else if (n == "ispunct") r = std::ispunct(ch); else if (n == "isspace") r = std::isspace(ch);
            else if (n == "isupper") r = std::isupper(ch); else r = std::isxdigit(ch);
            ret(c, r != 0); return true;
        }
    }
    if (n == "tolower") { ret(c, static_cast<uint32_t>(std::tolower(static_cast<int>(arg(c, 0))))); return true; }
    if (n == "toupper") { ret(c, static_cast<uint32_t>(std::toupper(static_cast<int>(arg(c, 0))))); return true; }
    // ---- math ----
    {
        struct M1 { const char *name; double (*f)(double); };
        static const M1 m1[] = { { "sqrt", std::sqrt }, { "sin", std::sin }, { "cos", std::cos }, { "tan", std::tan },
            { "asin", std::asin }, { "acos", std::acos }, { "atan", std::atan }, { "sinh", std::sinh }, { "cosh", std::cosh },
            { "tanh", std::tanh }, { "exp", std::exp }, { "log", std::log }, { "log10", std::log10 }, { "fabs", std::fabs },
            { "floor", std::floor }, { "ceil", std::ceil }, { "trunc", std::trunc }, { "round", std::round }, { "__c6xabi_trunc", std::trunc }, { "__c6xabi_nround", std::round },
            { "log2", std::log2 }, { "cbrt", std::cbrt }, { "exp2", std::exp2 }, { "log1p", std::log1p }, { "expm1", std::expm1 } };
        for (const M1 &m : m1) if (n == m.name) { retDouble(c, m.f(argDouble(c, 0))); return true; }
        struct M2 { const char *name; double (*f)(double, double); };
        static const M2 m2[] = { { "atan2", std::atan2 }, { "pow", std::pow }, { "fmod", std::fmod }, { "hypot", std::hypot } };
        for (const M2 &m : m2) if (n == m.name) { retDouble(c, m.f(argDouble(c, 0), argDouble(c, 1))); return true; }
        if (n == "ldexp") { retDouble(c, std::ldexp(argDouble(c, 0), static_cast<int>(arg(c, 1)))); return true; }
        if (n == "frexp") { int e = 0; double r = std::frexp(argDouble(c, 0), &e); c.store32(arg(c, 1), static_cast<uint32_t>(e)); retDouble(c, r); return true; }
        if (n == "modf") { double ip = 0; double r = std::modf(argDouble(c, 0), &ip); uint64_t v; std::memcpy(&v, &ip, 8); c.store64(arg(c, 1), v); retDouble(c, r); return true; }
        if (n == "sqrtf") { retFloat(c, std::sqrt(argFloat(c, 0))); return true; }
        if (n == "fabsf") { retFloat(c, std::fabs(argFloat(c, 0))); return true; }
        if (n == "floorf") { retFloat(c, std::floor(argFloat(c, 0))); return true; }
        if (n == "ceilf") { retFloat(c, std::ceil(argFloat(c, 0))); return true; }
    }
    // ---- setjmp / longjmp: the callee-saved registers, the stack and the return ----
    if (n == "setjmp" || n == "_setjmp") {
        uint32_t env = arg(c, 0);
        static const int saved[] = { 10, 11, 12, 13, 14, 15, Cpu::B + 10, Cpu::B + 11, Cpu::B + 12, Cpu::B + 13, Cpu::B + 14, Cpu::B + 15, Cpu::B3 };
        for (int i = 0; i < 13; i++) c.store32(env + 4 * i, c.reg(saved[i]));
        ret(c, 0);
        return true;
    }
    if (n == "longjmp") {
        uint32_t env = arg(c, 0), val = arg(c, 1);
        static const int saved[] = { 10, 11, 12, 13, 14, 15, Cpu::B + 10, Cpu::B + 11, Cpu::B + 12, Cpu::B + 13, Cpu::B + 14, Cpu::B + 15, Cpu::B3 };
        for (int i = 0; i < 13; i++) c.setReg(saved[i], c.load32(env + 4 * i));
        ret(c, val == 0 ? 1 : val);           // and B3 is now setjmp's return address
        return true;
    }
    // ---- the C++ ABI ----
    if (n == "_Znwm" || n == "_Znam" || n == "_Znwj" || n == "_Znaj") { ret(c, allocate(c, arg(c, 0))); return true; }
    if (n == "_ZdlPv" || n == "_ZdaPv" || n == "_ZdlPvm" || n == "_ZdaPvm" || n == "_ZdlPvj" || n == "_ZdaPvj") { release(c, arg(c, 0)); return true; }
    if (n == "__cxa_guard_acquire") { ret(c, c.load8(arg(c, 0)) == 0); return true; }
    if (n == "__cxa_guard_release") { c.store8(arg(c, 0), 1); return true; }
    if (n == "__cxa_guard_abort") { return true; }
    if (n == "__cxa_atexit") { AtExit a; a.fn = arg(c, 0); a.arg = arg(c, 1); atExit_.push_back(a); ret(c, 0); return true; }
    if (n == "__dynamic_cast") { ret(c, dynamicCast(c, arg(c, 0), arg(c, 1), arg(c, 2))); return true; }
    if (n == "__cxa_pure_virtual") c.fault("a pure virtual function was called");
    if (n == "__cxa_deleted_virtual") c.fault("a deleted virtual function was called");
    if (n == "__cxa_allocate_exception") {
        // 32 bytes of header before the object, as the Itanium layout has,
        // though the header's contents are kept on this side.
        uint32_t base = allocate(c, arg(c, 0) + 32);
        Exc e; e.obj = base + 32;
        excs_.push_back(e);
        ret(c, e.obj);
        return true;
    }
    if (n == "__cxa_free_exception") {
        uint32_t obj = arg(c, 0);
        for (size_t i = 0; i < excs_.size(); i++) if (excs_[i].obj == obj) { excs_.erase(excs_.begin() + static_cast<long>(i)); break; }
        release(c, obj - 32);
        return true;
    }
    if (n == "__cxa_throw") {
        uint32_t obj = arg(c, 0);
        Exc *e = excFor(obj);
        if (e == nullptr) c.fault("__cxa_throw of something __cxa_allocate_exception did not give");
        e->ti = arg(c, 1); e->dtor = arg(c, 2); e->rethrown = false;
        // The thrower's frame is the caller's: its return address is B3, its
        // frame pointer A15, its stack pointer B15.
        throwFrom(c, obj, c.reg(Cpu::B3), c.reg(Cpu::A15), c.reg(Cpu::B15));
        return true;
    }
    if (n == "__cxa_rethrow") {
        if (caught_.empty()) terminate(c, "rethrow with no exception being handled");
        uint32_t obj = caught_.back();
        Exc *e = excFor(obj);
        if (e != nullptr) e->rethrown = true;
        throwFrom(c, obj, c.reg(Cpu::B3), c.reg(Cpu::A15), c.reg(Cpu::B15));
        return true;
    }
    if (n == "_Unwind_Resume" || n == "__cxa_end_cleanup") {
        // A cleanup pad is done: carry on with the descriptor after its own, in the frame the pad
        // ran in - A15's, with B15 as the pad left it. TI's entry takes nothing and resumes the
        // exception the landing recorded; the Itanium one is given it.
        uint32_t obj = n[0] == '_' && n[1] == 'U' ? arg(c, 0) : cleanupExc_;
        Exc *e = excFor(obj);
        if (e == nullptr) c.fault("_Unwind_Resume of an unknown exception");
        if (e->cleanupNext == 0) c.fault("_Unwind_Resume from a pad no cleanup descriptor named");
        unwindTo(c, *e, e->cleanupPc, c.reg(Cpu::A15), c.reg(Cpu::B15), e->cleanupNext);
        return true;
    }
    if (n == "__cxa_begin_catch") {
        uint32_t obj = arg(c, 0);
        Exc *e = excFor(obj);
        if (e == nullptr) c.fault("__cxa_begin_catch of an unknown exception");
        e->handlers++;
        e->rethrown = false;
        caught_.push_back(obj);
        ret(c, e->adjusted);
        return true;
    }
    if (n == "__cxa_end_catch") {
        if (caught_.empty()) c.fault("__cxa_end_catch with nothing caught");
        uint32_t obj = caught_.back();
        caught_.pop_back();
        Exc *e = excFor(obj);
        if (e == nullptr) return true;
        if (--e->handlers == 0 && !e->rethrown) {
            uint32_t dtor = e->dtor;
            if (dtor != 0) c.callback(dtor, obj, 0);
            for (size_t i = 0; i < excs_.size(); i++) if (excs_[i].obj == obj) { excs_.erase(excs_.begin() + static_cast<long>(i)); break; }
            release(c, obj - 32);
        }
        return true;
    }
    // ---- arrays, Itanium 3.3.3 as the C6000 EABI takes it: ctor and cctor answer the array ----
    if (n == "__cxa_vec_ctor" || n == "__cxa_vec_cctor") {
        const bool copy = n == "__cxa_vec_cctor";
        uint32_t base = arg(c, 0), src = copy ? arg(c, 1) : 0, count = arg(c, copy ? 2 : 1), size = arg(c, copy ? 3 : 2), ctor = arg(c, copy ? 4 : 3);
        for (uint32_t i = 0; i < count && ctor != 0; i++) c.callback(ctor, base + i * size, src + i * size);
        ret(c, base);
        return true;
    }
    if (n == "__cxa_vec_dtor" || n == "__cxa_vec_cleanup") {
        uint32_t base = arg(c, 0), count = arg(c, 1), size = arg(c, 2), dtor = arg(c, 3);
        for (uint32_t i = count; i-- > 0 && dtor != 0; ) c.callback(dtor, base + i * size, 0);
        return true;
    }
    // The cookie is the count in the word before the array, inside the padding the caller sized.
    if (n == "__cxa_vec_new" || n == "__cxa_vec_new2" || n == "__cxa_vec_new3") {
        uint32_t count = arg(c, 0), size = arg(c, 1), pad = arg(c, 2), ctor = arg(c, 3), alloc = n.size() > 13 ? arg(c, 5) : 0;
        uint32_t block = alloc != 0 ? c.callback(alloc, count * size + pad, 0) : allocate(c, count * size + pad);
        uint32_t base = block + pad;
        if (pad != 0) c.store32(base - 4, count);
        for (uint32_t i = 0; i < count && ctor != 0; i++) c.callback(ctor, base + i * size, 0);
        ret(c, base);
        return true;
    }
    if (n == "__cxa_vec_delete" || n == "__cxa_vec_delete2" || n == "__cxa_vec_delete3") {
        uint32_t base = arg(c, 0), size = arg(c, 1), pad = arg(c, 2), dtor = arg(c, 3), dealloc = n.size() > 16 ? arg(c, 4) : 0;
        if (base == 0) return true;
        uint32_t count = pad != 0 ? c.load32(base - 4) : 0;
        for (uint32_t i = count; i-- > 0 && dtor != 0; ) c.callback(dtor, base + i * size, 0);
        if (dealloc != 0) c.callback(dealloc, base - pad, count * size + pad); else release(c, base - pad);
        return true;
    }
    // ---- cl6x's <iostream>, over the objects streamObjects() lays out ----
    if (n == "_ZNSt8ios_base4InitC1Ev" || n == "_ZNSt8ios_base4InitC2Ev") return true;
    if (n == "_ZNSt8ios_base4InitD1Ev" || n == "_ZNSt8ios_base4InitD2Ev") { std::fflush(stdout); std::fflush(stderr); return true; }
    if (n == "_ZNSt6localeC1ERKS_" || n == "_ZNSt6localeC2ERKS_") { c.store32(arg(c, 0), c.load32(arg(c, 1))); return true; }
    if (n == "_ZNSt6localeD1Ev" || n == "_ZNSt6localeD2Ev") return true;
    if (n == "_ZNSt4priv11_GetFacetIdEPKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE") { ret(c, symbolAt(c, "__vm6747_num_put_id")); return true; }
    if (n == "_ZNKSt6locale12_M_use_facetERKNS_2idE" || n == "_ZNKSt6locale11_M_get_facetERKNS_2idE") {
        if (c.load32(arg(c, 1)) == 1) { ret(c, symbolAt(c, "__vm6747_num_put")); return true; }
        if (n[14] == '1') { ret(c, 0); return true; }   // _M_get_facet answers null
        terminate(c, "use_facet of a facet the runtime does not provide: num_put<char> is the one here");
    }
    if (n == "_ZNSt8ios_base16_M_throw_failureEv") terminate(c, "ios_base::failure");
    if (n == "__vm6747_sb_sync") { if (FILE *f = hostOf(c, arg(c, 0))) std::fflush(f); ret(c, 0); return true; }
    if (n == "__vm6747_sb_overflow") {
        uint32_t ch = arg(c, 1);
        if (ch != 0xffffffffu) { if (FILE *f = hostOf(c, arg(c, 0))) std::fputc(static_cast<int>(ch & 0xff), f); }
        ret(c, ch == 0xffffffffu ? 0 : ch & 0xff);
        return true;
    }
    if (n == "__vm6747_sb_xsputn" || n == "__vm6747_sb_xsputnc") {
        uint32_t k = arg(c, 2);
        std::string s;
        if (n.size() == 19) s.assign(k, static_cast<char>(arg(c, 1)));
        else for (uint32_t i = 0; i < k; i++) s += static_cast<char>(c.load8(arg(c, 1) + i));
        if (FILE *f = hostOf(c, arg(c, 0))) std::fwrite(s.data(), 1, s.size(), f);
        ret(c, k);
        return true;
    }
    if (n.compare(0, 17, "__vm6747_num_put_") == 0) {
        const std::string kind = n.substr(17);
        const uint32_t ios = arg(c, 2), flags = c.load32(ios + 4);
        const bool hex = (flags & 0x238) == 0x210;    // showbase and hex: internal padding goes after the 0x
        if (kind == "bool" && (flags & 0x100)) numPut(c, arg(c, 4) & 0xff ? "true" : "false", false);
        else if (kind == "bool" || kind == "long") numPut(c, integerText(static_cast<int32_t>(arg(c, 4)), true, 32, flags), hex);
        else if (kind == "ulong") numPut(c, integerText(arg(c, 4), false, 32, flags), hex);
        else if (kind == "llong") numPut(c, integerText(static_cast<int64_t>(argWide(c, 4)), true, 64, flags), hex);
        else if (kind == "ullong") numPut(c, integerText(static_cast<int64_t>(argWide(c, 4)), false, 64, flags), hex);
        else if (kind == "pointer") { char b[16]; std::snprintf(b, sizeof b, "0x%08x", arg(c, 4)); numPut(c, b, true); }
        else numPut(c, floatText(argDouble(c, 4), flags, static_cast<int32_t>(c.load32(ios + 24))), false);
        return true;
    }
    if (n == "__cxa_get_exception_ptr") { Exc *e = excFor(arg(c, 0)); ret(c, e != nullptr ? e->adjusted : arg(c, 0)); return true; }
    if (n == "__cxa_bad_cast") terminate(c, "bad dynamic_cast to a reference");
    if (n == "__cxa_bad_typeid") terminate(c, "typeid of a null pointer");
    if (n == "_ZSt9terminatev") terminate(c, "std::terminate()");
    if (n == "__gxx_personality_v0") c.fault("'__gxx_personality_v0' reached: the unwinder here is the runtime's own");
    if (n == "_ZNKSt9type_infoeqERKS_") { ret(c, sameType(c, arg(c, 0), arg(c, 1))); return true; }
    if (n == "_ZNKSt9type_infoneERKS_") { ret(c, !sameType(c, arg(c, 0), arg(c, 1))); return true; }
    if (n == "_ZNKSt9type_info4nameEv") { ret(c, c.load32(arg(c, 0) + 4)); return true; }
    if (n == "_ZNKSt9type_info6beforeERKS_") { ret(c, c.readString(c.load32(arg(c, 0) + 4)) < c.readString(c.load32(arg(c, 1) + 4))); return true; }
    // ---- the EABI helpers ----
    if (n == "__c6xabi_divi") { int32_t a = static_cast<int32_t>(c.reg(Cpu::A4)), b = static_cast<int32_t>(c.reg(Cpu::B4)); if (b == 0) c.fault("division by zero"); ret(c, static_cast<uint32_t>(b == -1 ? -a : a / b)); return true; }
    if (n == "__c6xabi_divu") { uint32_t a = c.reg(Cpu::A4), b = c.reg(Cpu::B4); if (b == 0) c.fault("division by zero"); ret(c, a / b); return true; }
    if (n == "__c6xabi_remi") { int32_t a = static_cast<int32_t>(c.reg(Cpu::A4)), b = static_cast<int32_t>(c.reg(Cpu::B4)); if (b == 0) c.fault("division by zero"); ret(c, static_cast<uint32_t>(b == -1 ? 0 : a % b)); return true; }
    if (n == "__c6xabi_remu") { uint32_t a = c.reg(Cpu::A4), b = c.reg(Cpu::B4); if (b == 0) c.fault("division by zero"); ret(c, a % b); return true; }
    if (n == "__c6xabi_divlli") { int64_t a = static_cast<int64_t>(c.pair(Cpu::A4)), b = static_cast<int64_t>(c.pair(Cpu::B4)); if (b == 0) c.fault("division by zero"); retWide(c, static_cast<uint64_t>(b == -1 ? -a : a / b)); return true; }
    if (n == "__c6xabi_divull") { uint64_t a = c.pair(Cpu::A4), b = c.pair(Cpu::B4); if (b == 0) c.fault("division by zero"); retWide(c, a / b); return true; }
    if (n == "__c6xabi_remlli") { int64_t a = static_cast<int64_t>(c.pair(Cpu::A4)), b = static_cast<int64_t>(c.pair(Cpu::B4)); if (b == 0) c.fault("division by zero"); retWide(c, static_cast<uint64_t>(b == -1 ? 0 : a % b)); return true; }
    if (n == "__c6xabi_remull") { uint64_t a = c.pair(Cpu::A4), b = c.pair(Cpu::B4); if (b == 0) c.fault("division by zero"); retWide(c, a % b); return true; }
    if (n == "__c6xabi_divf") { retFloat(c, argFloat(c, 0) / argFloat(c, 1)); return true; }
    if (n == "__c6xabi_divd") { retDouble(c, argDouble(c, 0) / argDouble(c, 1)); return true; }
    if (n == "__c6xabi_fixfu") { float f = argFloat(c, 0); ret(c, f != f || f <= 0 ? 0 : f >= 4294967295.0f ? 0xffffffffu : static_cast<uint32_t>(f)); return true; }
    if (n == "__c6xabi_fixdu") { double d = argDouble(c, 0); ret(c, d != d || d <= 0 ? 0 : d >= 4294967295.0 ? 0xffffffffu : static_cast<uint32_t>(d)); return true; }
    if (n == "__c6xabi_fixflli") { float f = argFloat(c, 0); retWide(c, static_cast<uint64_t>(f != f ? 0 : static_cast<int64_t>(f))); return true; }
    if (n == "__c6xabi_fixdlli") { double d = argDouble(c, 0); retWide(c, static_cast<uint64_t>(d != d ? 0 : static_cast<int64_t>(d))); return true; }
    if (n == "__c6xabi_fixfull") { float f = argFloat(c, 0); retWide(c, f != f || f <= 0 ? 0 : static_cast<uint64_t>(f)); return true; }
    if (n == "__c6xabi_fixdull") { double d = argDouble(c, 0); retWide(c, d != d || d <= 0 ? 0 : static_cast<uint64_t>(d)); return true; }
    if (n == "__c6xabi_fltllif") { retFloat(c, static_cast<float>(static_cast<int64_t>(argWide(c, 0)))); return true; }
    if (n == "__c6xabi_fltllid") { retDouble(c, static_cast<double>(static_cast<int64_t>(argWide(c, 0)))); return true; }
    if (n == "__c6xabi_fltullf") { retFloat(c, static_cast<float>(argWide(c, 0))); return true; }
    if (n == "__c6xabi_fltulld") { retDouble(c, static_cast<double>(argWide(c, 0))); return true; }
    return false;
}
