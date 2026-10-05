#!/bin/sh
# oracle/win/calibrate.sh [--ccs] - vm6747sim calibrated on the Windows box, from this checkout.
# Builds this checkout's src/ there with msvc/build.cmd (Visual Studio 2022), runs every image in oracle/ship -
# the 49 (img, cpp11img) and the 32 from cl6x 8.2.2 (img822/img) - and brings the outputs and cycle counts back
# to oracle/ship/win-calib/, where they are held to CCS 5.5's: its stored results, or with --ccs a fresh run of
# CCS 5.5's C6747 cycle-accurate simulator on the box (cycle.CPU, through DSS and C:\simrev\runca.js - slow:
# one DSS session per image). Run it on the Mac, from anywhere; the box is ssh GRA@192.168.100.84.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
BOX=${BOX:-GRA@192.168.100.84}
RDIR='C:/simrev/calib'
CCS=0; [ "${1:-}" = "--ccs" ] && CCS=1
SHIP="$HERE/oracle/ship"; OUT="$SHIP/win-calib"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/calib.XXXXXX")
mkdir -p "$WORK/calib/imgs" "$OUT"
for f in "$SHIP"/img/*.out "$SHIP"/cpp11img/*.out "$SHIP"/img822/img/*.out; do
    case "$(basename "$f")" in *" "*|._*) continue ;; esac
    cp "$f" "$WORK/calib/imgs/"
done
cp -R "$HERE/src" "$HERE/msvc" "$WORK/calib/"
cat > "$WORK/calib/run.ps1" <<'PS'
param([int]$Ccs = 0)
$W = "C:\simrev\calib"; $Out = "$W\out"; New-Item -ItemType Directory -Force $Out | Out-Null
$Sim = "$W\vm6747.exe"; $Dss = "C:\ti\ccsv5\ccs_base\scripting\bin\dss.bat"
$Res = "$Out\results.txt"; Set-Content $Res ""
foreach ($img in (Get-ChildItem "$W\imgs\*.out" | Sort-Object Name)) {
  $n = $img.BaseName; $o = "$Out\$n"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  cmd /c "`"$Sim`" --run -c `"$($img.FullName)`" > `"$o.sim.txt`" 2> `"$o.sim.err`""
  $sw.Stop()
  $line = "$n sim_ms $($sw.ElapsedMilliseconds)"
  if ($Ccs -eq 1) {
    $r = (& $Dss "C:\simrev\runca.js" "C:\simrev\c6747ca-windows.ccxml" $img.FullName "$o.ccs1.cio" 1 1500000 2>&1 | Select-String "^RESULT").Line
    if ($r -match 'count=(\d+) pc=(0x[0-9a-f]+) exit=(0x[0-9a-f-]+).*wall_ms=(\d+)') { $line += " ccs1 $($Matches[1]) $($Matches[2] -eq $Matches[3]) $($Matches[4])" } else { $line += " ccs1 none" }
  }
  Add-Content $Res $line
}
Add-Content $Res "DONE"
PS
( cd "$WORK" && COPYFILE_DISABLE=1 tar czf calib.tgz --exclude '._*' calib ) || exit 1
echo "calibrate: copying $(ls "$WORK/calib/imgs" | wc -l | tr -d ' ') images and the sources to $BOX"
ssh "$BOX" "powershell -NoProfile -Command \"Remove-Item -Recurse -Force C:\\simrev\\calib -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force C:\\simrev | Out-Null\"" || exit 1
scp -q "$WORK/calib.tgz" "$BOX:C:/simrev/calib.tgz" || exit 1
ssh "$BOX" "tar -xzf C:/simrev/calib.tgz -C C:/simrev" || exit 1
echo "calibrate: building with MSVC"
ssh "$BOX" "C:\\simrev\\calib\\msvc\\build.cmd" > "$OUT/build.log" 2>&1 || { echo "calibrate: the build failed - see $OUT/build.log"; exit 1; }
echo "calibrate: running (--ccs: $CCS)"
ssh "$BOX" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\simrev\\calib\\run.ps1 -Ccs $CCS" || exit 1
ssh "$BOX" "tar -czf C:/simrev/calib-out.tgz -C C:/simrev/calib out" || exit 1
scp -q "$BOX:C:/simrev/calib-out.tgz" "$WORK/" || exit 1
rm -rf "$OUT/out" && tar xzf "$WORK/calib-out.tgz" -C "$OUT" || exit 1
rm -rf "$WORK"
python3 "$HERE/oracle/win/report.py" "$SHIP" | tee "$OUT/report.txt"
