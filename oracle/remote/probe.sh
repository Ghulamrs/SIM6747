#!/bin/sh
# probe.sh [steps] - the Linux box's half of oracle/run.sh, run in the directory it was unpacked to.
# Everything here is TI's own CCS 5.5 tools, run as shipped:
#   inventory    what this CCS 5.5 holds: compiler, simulator configurations, clock events, documents
#   words        dis6x's reading of words.asm, the decoder corpus (gen-words.py)
#   programs     each programs/*.c built by cl6x 7.4.4 at -O2 and without -O, linked with rts6740_elf.lib;
#                its map, dis6x listing, ofd6x dump, hex6x images, and trace.js's step-by-step state
#                on the C6747 cycle-accurate simulator, with its CIO output
# Results go to out/, which run.sh copies back.
set -u
STEPS=${1:-20000}
W=$(cd "$(dirname "$0")" && pwd)
CCS=${CCS55:-$HOME/ti/ccsv5}
CG=$CCS/tools/compiler/c6000_7.4.4
DSS=$CCS/ccs_base/scripting/bin/dss.sh
CCXML=$W/c6747ca-linux.ccxml
cd "$W" || exit 1
rm -rf out && mkdir -p out/programs
PATH=$CG/bin:$PATH; export PATH

# ---- inventory -------------------------------------------------------------------------------
{
    echo "box linux  $(uname -a)"
    echo "ccs $CCS"
    ls "$CCS/tools/compiler" 2>&1
    cl6x --help 2>&1 | head -3
    echo "--- simulator configurations naming the C674x or C6747"
    grep -rl --include=*.xml -e "C674x" -e "C6747" "$CCS/ccs_base/common/targetdb" 2>/dev/null | head -50
    echo "--- documents"
    find "$CCS" \( -iname '*.pdf' -o -iname '*.chm' \) 2>/dev/null | grep -i -e c6 -e c67 -e c64 -e sim -e spru | head -80
} > out/inventory.txt 2>&1
# every targetdb file naming a C674x CPU-only or functional simulator, for the ccxml of those configurations
mkdir -p out/targetdb
grep -rl --include=*.xml -e "C674x CPU Cycle Accurate" -e "C6747 Device Functional" -e "tisim_c674x" \
    "$CCS/ccs_base/common/targetdb" 2>/dev/null | head -20 | while read f; do cp "$f" out/targetdb/; done

# ---- words -----------------------------------------------------------------------------------
if [ -f words.asm ]; then
    cl6x -mv6740 --abi=eabi -c words.asm --obj_directory=out > out/words.build.log 2>&1
    dis6x out/words.obj > out/words.dis 2>&1
    dis6x --help > out/dis6x-help.txt 2>&1
fi

# ---- programs --------------------------------------------------------------------------------
LINK="-z --heap_size=0x8000 --stack_size=0x2000 --rom_model $W/C6747.cmd -l$CG/lib/rts6740_elf.lib"
for c in programs/*.c; do
    [ -f "$c" ] || continue
    n=$(basename "$c" .c)
    for lvl in O2 O0; do
        b=out/programs/$n.$lvl
        opt=-O2; [ $lvl = O0 ] && opt=
        cl6x -mv6740 --abi=eabi $opt ${TI_COMPRESS:-} --symdebug:none -I"$CG/include" --obj_directory=out/programs \
            "$c" $LINK -m $b.map -o $b.out > $b.build.log 2>&1
        [ -f $b.out ] || { echo "PROGRAM $n.$lvl build=FAILED"; continue; }
        dis6x $b.out > $b.dis 2>&1
        ofd6x -v $b.out > $b.ofd 2>&1
        hex6x -q --ti_txt $b.out -o $b.ti.txt > /dev/null 2>&1
        hex6x -q -i $b.out -o $b.intel.hex > /dev/null 2>&1
        hex6x -q -m $b.out -o $b.srec > /dev/null 2>&1
        hex6x -q -a $b.out -o $b.ascii.hex > /dev/null 2>&1
        hex6x -q -t $b.out -o $b.tagged.hex > /dev/null 2>&1
        r=$("$DSS" "$W/trace.js" "$CCXML" "$W/$b.out" "$W/$b.trace" "$W/$b.cio" "$STEPS" 2>&1 | grep '^TRACE')
        echo "PROGRAM $n.$lvl build=ok $r"
    done
done | tee out/programs.txt
