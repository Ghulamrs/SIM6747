#!/bin/sh
# tests/c6x-cio-text.sh - CIO files in text and binary mode, as CCS 5.5's host answers on each system (measured
# 2026-10-07): on Windows a file opened without O_BINARY is the C runtime's text mode, LF written as CR LF and
# CR LF read back as LF; on Linux nothing is translated. Run from the repository root; needs perl.
set -u
VM=${VM:-./vm6747.exe}
case "$VM" in /*|[A-Za-z]:*) ;; *) VM=$(pwd)/$VM ;; esac
T=${TMPDIR:-/tmp}/c6x-cio.$$
mkdir -p "$T" && trap 'rm -rf "$T"' EXIT
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) win=1 ;; *) win=0 ;; esac
# The driver, asm6x's words for it: copy each request of the table at 0x80003000 into _CIOBUF_ (0x80002000),
# branch through C$$IO$$ (0x80001068), and at "seen" (0x80001060) hold the answer word in A3 and the first data
# word in A4; a table entry of 0 words branches to C$$EXIT (0x80001070).
perl - "$T/cio.out" <<'PL'
use strict;
my @code = map { hex } qw(05180028 05400068 00a83664 90000e10 00008000 05900028 05c00068 e0100000
    01283664 012c3674 0087e058 80000010 00008000 0188222a 01c0006a e0100000 8c6e050a 05900028 05c00068
    01ac2264 022c6264 02ac8264 00006000 e0208000 0ffff510 00008000 000c0362 00008000 00000000 00000210
    00008000 00000000);
sub req { my ($cmd, $parm, $data) = @_; my $r = pack('V', length $data) . chr($cmd) . $parm . $data;
          $r .= "\0" x (-length($r) % 4); return pack('V', length($r) / 4) . $r; }
sub openr { req(0xF0, pack('vvV', $_[0], $_[1], 0), "$_[2]\0") }
sub writer { req(0xF3, pack('vvV', $_[0], length $_[1], 0), $_[1]) }
sub readr { req(0xF2, pack('vvV', $_[0], $_[1], 0), '') }
sub seekr { req(0xF4, pack('vVv', $_[0], $_[1], $_[2]), '') }
sub closer { req(0xF1, pack('vvV', $_[0], 0, 0), '') }
my $table = openr(3, 0x601, 't1.txt') . writer(3, "a\nb\n") . seekr(3, 0, 1) . closer(3)
    . openr(4, 0, 'r.txt') . readr(4, 100) . seekr(4, 0, 1) . seekr(4, 0, 0) . readr(4, 2) . seekr(4, 0, 1) . closer(4)
    . openr(5, 0x8601, 't3.txt') . writer(5, "a\nb\n") . closer(5) . pack('V', 0);
my $text = pack('V*', @code);
# An ELF32 for the C6000: .text at 0x80001000, the table at 0x80003000, the three symbols the host looks for.
my @sym = (['C$$IO$$', 0x80001068, 1], ['C$$EXIT', 0x80001070, 1], ['_CIOBUF_', 0x80002000, 2]);
my $strtab = "\0"; my $symtab = "\0" x 16;
for (@sym) { $symtab .= pack('VVVCCv', length $strtab, $_->[1], 0, 0x10, 0, $_->[2]); $strtab .= "$_->[0]\0"; }
my $shstr = "\0.text\0.data\0.symtab\0.strtab\0.shstrtab\0";
my @body = ($text, $table, $symtab, $strtab, $shstr);
my $off = 52; my @at; for (@body) { push @at, $off; $off += length; $off += -$off % 4; }
my @sh = ([0, 0, 0, 0, 0, 0, 0, 0, 0, 0], [1, 1, 6, 0x80001000, $at[0], length $body[0], 0, 0, 32, 0],
    [7, 1, 3, 0x80003000, $at[1], length $body[1], 0, 0, 4, 0], [13, 2, 0, 0, $at[2], length $body[2], 4, 1, 4, 16],
    [21, 3, 0, 0, $at[3], length $body[3], 0, 0, 1, 0], [29, 3, 0, 0, $at[4], length $body[4], 0, 0, 1, 0]);
my $f = "\x7fELF\x01\x01\x01" . "\0" x 9 . pack('vvVVVVVvvvvvv', 2, 140, 1, 0x80001000, 0, $off, 0, 52, 0, 0, 40, 6, 5);
for my $i (0 .. $#body) { $f .= "\0" x ($at[$i] - length $f) . $body[$i]; }
$f .= "\0" x ($off - length $f) . join('', map { pack('V10', @$_) } @sh);
open my $o, '>:raw', $ARGV[0] or die; print $o $f; close $o;
PL
printf 'a\r\nb\r\n' > "$T/r.txt"
(cd "$T" && "$VM" --run cio.out -c --trace trace.txt > out.txt 2>&1)
# A3 is the answer; A4 the first data bytes, held for the two reads and only as far as each answered.
got=$(awk -v w=$win '$1 == "S" && $3 == "80001060" { n++; d = n == 6 ? " " $8 : n == 9 ? " " substr($8, w ? 7 : 5) : "";
      printf "%s%s ", $7, d }' "$T/trace.txt" 2>/dev/null)
size() { [ -f "$1" ] && wc -c < "$1" | tr -d ' ' || echo none; }
grep -q 'exit=C\$\$EXIT' "$T/out.txt" && ended=exit || ended=none
got="$got| t1 $(size "$T/t1.txt") t3 $(size "$T/t3.txt") $ended"
if [ $win = 1 ]; then
    want='00000003 00000004 00000006 00000000 00000003 00000004 0a620a61 00000006 00000000 00000001 61 '
    want="${want}00000002 00000000 00000003 00000004 00000000 | t1 6 t3 4 exit"
else
    want='00000003 00000004 00000004 00000000 00000003 00000006 620a0d61 00000006 00000000 00000002 0d61 '
    want="${want}00000002 00000000 00000003 00000004 00000000 | t1 4 t3 4 exit"
fi
if [ "$got" = "$want" ]; then echo "c6x-cio-text: ok ($([ $win = 1 ] && echo Windows text mode || echo no translation))"; exit 0; fi
echo "c6x-cio-text: FAILED"; echo "  want $want"; echo "  got  $got"; cat "$T/out.txt"; exit 1
