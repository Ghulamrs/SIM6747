#!/bin/sh
# oracle/run.sh linux|windows|both [steps] - run from your own Terminal on the Mac.
# Ships the oracle kit to the box, runs TI's CCS 5.5 tools there (remote/probe.sh or probe.cmd), and
# brings the results back to oracle/results/<box>/: the inventory, dis6x's reading of the decoder corpus,
# and for each program its .out, map, dis6x and ofd6x listings, hex6x images, the simulator's
# step-by-step trace and its CIO output. sim6747 is then held to these (oracle/compare.sh). C6747.cmd and the two ccxml are in oracle/c6747/.
#
#   LINUX  ec2-user@52.202.164.123        KEY  ~/Documents/_NEW_/myMorningWalk.pem
#   WIN    GRA@192.168.100.84             WINDIR C:/Users/GRA/Documents/SIM6747
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BOX=${1:-both}
STEPS=${2:-20000}
LINUX=${LINUX:-ec2-user@52.202.164.123}
KEY=${KEY:-$HOME/Documents/_NEW_/myMorningWalk.pem}
WIN=${WIN:-GRA@192.168.100.84}
WINDIR=${WINDIR:-C:/Users/GRA/Documents/SIM6747}
C6747=$HERE/c6747
WORK=${TMPDIR:-/tmp}/sim6747-oracle.$$
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/oracle/programs"
cp "$HERE"/remote/* "$WORK/oracle/"
cp "$C6747/C6747.cmd" "$C6747/c6747ca-windows.ccxml" "$C6747/c6747ca-linux.ccxml" "$WORK/oracle/"
cp "$HERE"/programs/*.c "$HERE"/programs/*.cpp "$WORK/oracle/programs/"
python3 "$HERE/gen-words.py" 4096 6747 > "$WORK/oracle/words.asm"
# --no-mac-metadata: no com.apple.provenance headers for GNU tar on the box to warn about
( cd "$WORK" && { COPYFILE_DISABLE=1 tar czf oracle.tgz --no-mac-metadata --exclude "._*" oracle 2>/dev/null || COPYFILE_DISABLE=1 tar czf oracle.tgz --exclude "._*" oracle; } )

linux() {
    echo "== linux: $LINUX"
    ssh -i "$KEY" -o ConnectTimeout=15 "$LINUX" "rm -rf ~/sim6747-oracle && mkdir -p ~/sim6747-oracle" || return 1
    scp -q -i "$KEY" "$WORK/oracle.tgz" "$LINUX:sim6747-oracle/" || return 1
    ssh -i "$KEY" "$LINUX" "cd ~/sim6747-oracle && tar xzf oracle.tgz && sh oracle/probe.sh $STEPS && tar czf out.tgz -C oracle out" || return 1
    rm -rf "$HERE/results/linux" && mkdir -p "$HERE/results/linux"
    scp -q -i "$KEY" "$LINUX:sim6747-oracle/out.tgz" "$WORK/linux.tgz" && tar xzf "$WORK/linux.tgz" -C "$HERE/results/linux" --strip-components 1
}
# The Windows box's ssh lands in PowerShell, so every step is handed to cmd in one double-quoted
# string: PowerShell 5 does not parse '&&' itself.
windows() {
    echo "== windows: $WIN"
    D=$(echo "$WINDIR" | tr / '\\')
    ssh -o ConnectTimeout=15 "$WIN" "cmd /c \"if not exist $D mkdir $D\"" || return 1
    scp -q "$WORK/oracle.tgz" "$WIN:$WINDIR/" || return 1
    ssh "$WIN" "cmd /c \"cd /d $D && (if exist oracle rmdir /s /q oracle) && tar xzf oracle.tgz && oracle\\probe.cmd $STEPS && tar czf out.tgz -C oracle out\"" || return 1
    rm -rf "$HERE/results/windows" && mkdir -p "$HERE/results/windows"
    scp -q "$WIN:$WINDIR/out.tgz" "$WORK/windows.tgz" && tar xzf "$WORK/windows.tgz" -C "$HERE/results/windows" --strip-components 1
}
case "$BOX" in
    linux) linux ;;
    windows) windows ;;
    both) linux; windows ;;
    *) echo "usage: run.sh linux|windows|both [steps]"; exit 2 ;;
esac
echo "results in $HERE/results"
