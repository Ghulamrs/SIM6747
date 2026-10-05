#include "C6xIsa.h"

namespace c6x {

enum FormatId : uint8_t {
#define C6X_FORMAT(name, ...) FMT_##name,
#define C6X_FORMATS
#include "C6xOpcodes.inc"
#undef C6X_FORMATS
#undef C6X_FORMAT
    FMT_count
};

static const Format kFormats[] = {
#define C6X_FORMAT(name, ...) { #name, __VA_ARGS__ },
#define C6X_FORMATS
#include "C6xOpcodes.inc"
#undef C6X_FORMATS
#undef C6X_FORMAT
};

static const Opcode kOpcodes[] = {
#define C6X_OPCODE(...) { __VA_ARGS__ },
#define C6X_OPCODES
#include "C6xOpcodes.inc"
#undef C6X_OPCODES
#undef C6X_OPCODE
};

// SPRUFE8 table 2-4: the control registers by MVC address. Some addresses name one register to read and
// another to write (IFR/ISR, EFR/ECR).
static const ControlReg kControl[] = {
    { "AMR", 0x00, 0x10, true, true },   { "CSR", 0x01, 0x10, true, true },
    { "IFR", 0x02, 0x1d, true, false },  { "ISR", 0x02, 0x10, false, true },
    { "ICR", 0x03, 0x10, false, true },  { "IER", 0x04, 0x10, true, true },
    { "ISTP", 0x05, 0x10, true, true },  { "IRP", 0x06, 0x10, true, true },
    { "NRP", 0x07, 0x10, true, true },   { "TSCL", 0x0a, 0x1f, true, true },
    { "TSCH", 0x0b, 0x1f, true, false }, { "ILC", 0x0d, 0x1f, true, true },
    { "RILC", 0x0e, 0x1f, true, true },  { "REP", 0x0f, 0x1f, true, true },
    { "PCE1", 0x10, 0x0f, true, false }, { "DNUM", 0x11, 0x1f, true, false },
    { "FADCR", 0x12, 0x1f, true, true }, { "FAUCR", 0x13, 0x1f, true, true },
    { "FMCR", 0x14, 0x1f, true, true },  { "SSR", 0x15, 0x1f, true, true },
    { "GPLYA", 0x16, 0x1f, true, true }, { "GPLYB", 0x17, 0x1f, true, true },
    { "GFPGFR", 0x18, 0x1f, true, true }, { "TSR", 0x1a, 0x1f, true, true },
    { "ITSR", 0x1b, 0x1f, true, true },  { "NTSR", 0x1c, 0x1f, true, true },
    { "EFR", 0x1d, 0x1f, true, false },  { "ECR", 0x1d, 0x1f, false, true },
    { "IERR", 0x1f, 0x1f, true, true },
};

const Format &format(unsigned i) { return kFormats[i]; }
unsigned formatCount() { return sizeof kFormats / sizeof kFormats[0]; }
const Opcode &opcode(unsigned i) { return kOpcodes[i]; }
unsigned opcodeCount() { return sizeof kOpcodes / sizeof kOpcodes[0]; }
const ControlReg *controlRegs(unsigned &count) { count = sizeof kControl / sizeof kControl[0]; return kControl; }

} // namespace c6x
