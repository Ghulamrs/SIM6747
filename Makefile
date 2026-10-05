# vm6747 - the VM6747 emulator: runs the C6000 assembly that c90 and cpp11
# emit for -arch tms6747, so that what they emit can be verified rather than
# only read. Built the way the compilers are: C++14, one translation unit at a
# time, -Wall -Wextra -Werror -pedantic under clang++ on the Mac and g++ on the
# box, and every object outside the checkout.

ifeq ($(origin CXX),default)
  ifneq ($(shell command -v clang++ 2>/dev/null),)
    CXX := clang++
  else
    CXX := g++
  endif
endif

CXXFLAGS = -std=c++14 -O2 -g -Wall -Wextra -Werror -pedantic

# Filtered on src/%.cpp, as the compilers' Makefiles are: macOS leaves "keep
# both" duplicates - `Cpu 2.cpp` beside `Cpu.cpp` - and $(wildcard) splits the
# name at the space before anything can test it, so requiring both the prefix
# and the suffix drops both halves and keeps every real source.
SRCS     = $(filter src/%.cpp,$(wildcard src/*.cpp))
OBJDIR  ?= ../build/Emulator/obj
OBJS     = $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(SRCS))
# BINDIR: where the finished program goes - here by default, and the one
# directory RStudio's workspace.mk names for everything it drives.
BINDIR  ?= .
TARGET   = $(BINDIR)/vm6747.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
