#!/bin/sh
# The fourth sandbox: TI's own C6000 tools on the Windows box, as a second
# oracle beside the emulator. Every .s the three compilers' tms6747 suites
# left behind - c90's, cpp11's, shalimar's programs and the Shalimar runtime
# cpp11 built - is shipped there, assembled by cl6x and linked by lnk6x
# against rts6740_elf_eh.lib. The emulator says whether a program computes
# the right answer; this says whether TI's assembler takes every instruction,
# constant and directive as written and whether every name the backend uses
# is one TI's runtime defines. It cannot run anything: CCS7 has no simulator.
#
# What must be on the box: the compiler at C:\ti\ccsv7\tools\compiler\
# ti-cgt-c6000_8.2.2 (found there 2026-09-14), and the exception-handling
# build of its runtime at C:\Users\GRA\Documents\VM6747\tilib, made once by
#   mklib --pattern=rts6740_elf_eh.lib --index=libc.a --parallel=4
#         --install_to=C:\Users\GRA\Documents\VM6747\tilib
# from the lib directory, with C:\ti\ccsv7\utils\bin (gmake) on the PATH.
# The shipped rts6740_elf.lib is the no-exceptions build: every C++ program
# that throws, and every one that reaches operator new, needs the other.
#
# tests/ti-nolink.txt names the programs that assemble but do not link, and
# why; anything else that fails to assemble or link fails this suite.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/vm6747-ti.$$
REMOTE='C:/Users/GRA/Documents/VM6747'
SHMRT="${SHMRT:-$HERE/../Compiler-Si/lib/shmrt-tms6747}"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/tisweep/c" "$WORK/tisweep/cxx" "$WORK/tisweep/shm" "$WORK/tisweep/shmrt"
# Only real case names: macOS leaves "name 2.s" duplicates beside the files it rewrites.
stage() { for f in "$1"/*.s; do case "$(basename "$f")" in *" "*) ;; *) [ -f "$f" ] && cp "$f" "$2/";; esac; done; }
stage "$HERE/../Compiler-Ci/tests/out-tms6747" "$WORK/tisweep/c"
stage "$HERE/../Compiler-Cppi/tests/out-tms6747" "$WORK/tisweep/cxx"
stage "$HERE/../Compiler-Si/tests/out-tms6747" "$WORK/tisweep/shm"
stage "$SHMRT" "$WORK/tisweep/shmrt"
cp "$HERE/tests/ti-sweep.cmd" "$HERE/tests/ti-link.cmd" "$HERE/tests/par.cmd" "$WORK/tisweep/"
total=$(ls "$WORK"/tisweep/*/*.s | wc -l | tr -d ' ')
( cd "$WORK" && COPYFILE_DISABLE=1 tar czf tisweep.tgz --exclude "._*" tisweep )
ssh windows "if not exist C:\\Users\\GRA\\Documents\\VM6747 mkdir C:\\Users\\GRA\\Documents\\VM6747" || exit 1
scp -q "$WORK/tisweep.tgz" "windows:$REMOTE/" || exit 1
ssh windows "cd C:\\Users\\GRA\\Documents\\VM6747 && (rmdir /s /q tisweep 2>nul & tar xzf tisweep.tgz && tisweep\\ti-sweep.cmd)" > "$WORK/sweep.out" 2>&1
tr -d '\r' < "$WORK/sweep.out" > "$WORK/sweep.txt"
if grep -q NO_EH_LIBRARY "$WORK/sweep.txt"; then echo "ti.sh: no rts6740_elf_eh.lib on the box - see the header"; exit 2; fi
if ! grep -q SWEEP_DONE "$WORK/sweep.txt"; then echo "ti.sh: the sweep did not finish:"; tail -5 "$WORK/sweep.txt"; exit 1; fi
ssh windows "cd C:\\Users\\GRA\\Documents\\VM6747 && tar czf tisweep-logs.tgz tisweep\\c\\*.log tisweep\\cxx\\*.log tisweep\\shm\\*.log tisweep\\shmrt\\*.log tisweep\\c\\*.lnk tisweep\\cxx\\*.lnk tisweep\\shm\\*.lnk" > /dev/null 2>&1
scp -q "windows:$REMOTE/tisweep-logs.tgz" "$WORK/" && mkdir -p "$WORK/logs" && tar xzf "$WORK/tisweep-logs.tgz" -C "$WORK/logs"

failed=0; nolink=0; expected=0
for f in $(grep '^FAILED' "$WORK/sweep.txt" | awk '{print $2}' | tr '\\' '/'); do
    failed=$((failed + 1)); echo "FAIL $f does not assemble:"
    tr -d '\r' < "$WORK/logs/tisweep/${f%.s}.log" | grep -A1 '\[E' | head -6 | sed 's/^/      /'
done
for f in $(grep '^NOLINK' "$WORK/sweep.txt" | awk '{print $2}' | tr '\\' '/'); do
    name=${f%.obj}
    if grep -q "^$name\b" "$HERE/tests/ti-nolink.txt"; then expected=$((expected + 1)); continue; fi
    nolink=$((nolink + 1)); echo "FAIL $name does not link:"
    tr -d '\r' < "$WORK/logs/tisweep/$name.lnk" | awk '/^ ---------/{on=1; next} /^[[:space:]]*$/{on=0} on{print "      " $1}' | head -8
done
# A recorded unlinkable that now links is a line to remove, and says so.
for name in $(grep -v '^#' "$HERE/tests/ti-nolink.txt" | awk 'NF {print $1}'); do
    if ! grep -qF "NOLINK $(echo "$name" | tr '/' '\\').obj" "$WORK/sweep.txt"; then
        nolink=$((nolink + 1)); echo "FAIL $name links now - take it out of tests/ti-nolink.txt"
    fi
done
echo "ti.sh: $total assembled, $failed refused; $((total - $(ls "$WORK"/tisweep/shmrt/*.s | wc -l | tr -d ' ') - nolink - expected)) linked, $expected recorded as not linking, $nolink unexpected"
[ "$failed" -eq 0 ] && [ "$nolink" -eq 0 ]
