#pragma once

// --debug: the emulator under Debug.h's shell. Returns the exit status: the program's once it ended, else 0.

#include <cstdint>
#include <string>

class Cpu;
class Runtime;
struct Program;

int debugRun(Cpu &cpu, Program &prog, Runtime &rt, uint32_t entry, const std::string &name);
