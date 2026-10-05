#!/bin/sh
# The Windows leg: relay the emulator's sources to the Windows box, build
# them there with cl (msvc/build.cmd), run every .s the two compilers' suites
# left in their tests/out-tms6747 directories on that binary, and require
# the same output and exit status as this machine's binary gives for the
# same assembly. The .s files are the Mac's, so what this checks is the
# emulator's portability, not the compilers'; the compilers' own Windows
# legs are their business.
#
# The box is `ssh windows` (see the windows-test-machine note); its shell
# is cmd, so build.cmd is run by its full path with no "cmd /c" in front,
# and the loop over the cases runs in Git bash there. MSVC writes CRLF on
# stdout, so the Windows output is normalised before the comparison.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${TMPDIR:-/tmp}/vm6747-windows.$$
REMOTE='C:/Users/GRA/Documents/VM6747'
VM="${VM:-$HERE/vm6747.exe}"
mkdir -p "$WORK/xwin/c" "$WORK/xwin/cxx" "$WORK/xwin/shm" "$WORK/xwin/shmrt"
# Only real case names: macOS leaves "name 2.s" duplicates beside the files
# it rewrites, and those are noise, not cases.
for f in "$HERE"/../Compiler-Ci/tests/out-tms6747/*.s; do case "$(basename "$f")" in *" "*) ;; *) cp "$f" "$WORK/xwin/c/";; esac; done
for f in "$HERE"/../Compiler-Cppi/tests/out-tms6747/*.s; do case "$(basename "$f")" in *" "*) ;; *) cp "$f" "$WORK/xwin/cxx/";; esac; done
# The Shalimar programs run beside the runtime cpp11 compiled for the C6000,
# a directory of .s the emulator takes whole - so it travels too.
SHMRT="${SHMRT:-$HERE/../../RStudio/bin/lib/shmrt-tms6747}"   # where the one binary directory puts it
for f in "$HERE"/../Compiler-Si/tests/out-tms6747/*.s; do case "$(basename "$f")" in *" "*) ;; *) cp "$f" "$WORK/xwin/shm/";; esac; done
[ -d "$SHMRT" ] && cp "$SHMRT"/*.s "$WORK/xwin/shmrt/"
cat > "$WORK/xwin/run.sh" <<'REMOTE_EOF'
cd "$(dirname "$0")"
VM="${VM:-../Emulator/vm6747.exe}"
for d in c cxx; do for s in $d/*.s; do b=${s%.s}; "$VM" "$s" > "$b.out" 2>&1 < /dev/null; echo $? > "$b.rc"; done; done
rm -f shmrt/._* shm/._* c/._* cxx/._*
for s in shm/*.s; do [ -f "$s" ] || continue; b=${s%.s}; "$VM" "$s" shmrt > "$b.out" 2>&1 < /dev/null; echo $? > "$b.rc"; done
REMOTE_EOF
( cd "$HERE/.." && tar czf "$WORK/emulator.tgz" --exclude='*.exe' --exclude='.git' --exclude='._*' Emulator )
( cd "$WORK" && COPYFILE_DISABLE=1 tar czf xwin.tgz --exclude "._*" xwin )
ssh windows "if not exist C:\\Users\\GRA\\Documents\\VM6747 mkdir C:\\Users\\GRA\\Documents\\VM6747" || exit 1
scp -q "$WORK/emulator.tgz" "$WORK/xwin.tgz" "windows:$REMOTE/" || exit 1
ssh windows "cd C:\\Users\\GRA\\Documents\\VM6747 && tar xzf emulator.tgz && tar xzf xwin.tgz && del /q Emulator\\._* Emulator\\src\\._* 2>nul & C:\\Users\\GRA\\Documents\\VM6747\\Emulator\\msvc\\build.cmd" || { echo "windows.sh: the build on the box failed"; exit 1; }
ssh windows "\"C:\\Program Files\\Git\\bin\\bash.exe\" -c \"cd /c/Users/GRA/Documents/VM6747/xwin && sh run.sh\"" || exit 1
ssh windows "cd C:\\Users\\GRA\\Documents\\VM6747 && tar czf xwin-out.tgz xwin" && scp -q "windows:$REMOTE/xwin-out.tgz" "$WORK/" || exit 1
mkdir -p "$WORK/win" && tar xzf "$WORK/xwin-out.tgz" -C "$WORK/win"
same=0; differ=0
for d in c cxx shm; do
    for s in "$WORK"/xwin/$d/*.s; do
        [ -f "$s" ] || continue
        b=$(basename "$s" .s)
        if [ "$d" = shm ]; then "$VM" "$s" "$WORK/xwin/shmrt" > "$WORK/xwin/$d/$b.mac" 2>&1 < /dev/null; mrc=$?
        else "$VM" "$s" > "$WORK/xwin/$d/$b.mac" 2>&1 < /dev/null; mrc=$?; fi
        wrc=$(cat "$WORK/win/xwin/$d/$b.rc")
        tr -d '\r' < "$WORK/win/xwin/$d/$b.out" > "$WORK/win/xwin/$d/$b.lf"
        if [ "$mrc" = "$wrc" ] && cmp -s "$WORK/xwin/$d/$b.mac" "$WORK/win/xwin/$d/$b.lf"; then same=$((same + 1))
        else differ=$((differ + 1)); echo "DIFF $d/$b: rc mac=$mrc windows=$wrc"; fi
    done
done
echo "windows.sh: $same identical, $differ differ (the Windows-built emulator against this one, same assembly)"
# The work directory stays when something differed, so the two outputs can be read.
if [ "$differ" -eq 0 ]; then rm -rf "$WORK"; else echo "kept $WORK"; fi
[ "$differ" -eq 0 ]
