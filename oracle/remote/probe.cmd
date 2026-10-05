@echo off
rem probe.cmd [steps] - the Windows box's half of oracle/run.sh, run in the directory it was
rem unpacked to; the same probes as probe.sh, with CCS 5.5 at C:\ti\ccsv5. Results go to out\.
setlocal enabledelayedexpansion
set STEPS=%1
if "%STEPS%"=="" set STEPS=20000
set W=%~dp0
set W=%W:~0,-1%
if "%CCS55%"=="" set CCS55=C:\ti\ccsv5
set CG=%CCS55%\tools\compiler\c6000_7.4.4
set DSS=%CCS55%\ccs_base\scripting\bin\dss.bat
set CCXML=%W%\c6747ca-windows.ccxml
set PATH=%CG%\bin;%PATH%
cd /d "%W%"
if exist out rmdir /s /q out
mkdir out\programs
mkdir out\targetdb

rem ---- inventory
(
  echo box windows
  ver
  echo ccs %CCS55%
  dir /b "%CCS55%\tools\compiler"
  echo --- simulator configurations naming the C674x or C6747
  findstr /s /m /c:"C674x" /c:"C6747" "%CCS55%\ccs_base\common\targetdb\*.xml"
  echo --- documents
  dir /s /b "%CCS55%\*.pdf" "%CCS55%\*.chm" 2>nul | findstr /i "c6 sim spru"
) > out\inventory.txt 2>&1
for /f "delims=" %%f in ('findstr /s /m /c:"C674x CPU Cycle Accurate" /c:"C6747 Device Functional" /c:"tisim_c674x" "%CCS55%\ccs_base\common\targetdb\*.xml" 2^>nul') do copy /y "%%f" out\targetdb\ >nul

rem ---- words
if exist words.asm (
  cl6x -mv6740 --abi=eabi -c words.asm --obj_directory=out > out\words.build.log 2>&1
  dis6x out\words.obj > out\words.dis 2>&1
  dis6x --help > out\dis6x-help.txt 2>&1
)

rem ---- programs
set LINK=-z --heap_size=0x8000 --stack_size=0x2000 --rom_model "%W%\C6747.cmd" -l"%CG%\lib\rts6740_elf.lib"
type nul > out\programs.txt
for %%c in (programs\*.c) do (
  for %%l in (O2 O0) do (
    set B=out\programs\%%~nc.%%l
    set OPT=-O2
    if "%%l"=="O0" set OPT=
    cl6x -mv6740 --abi=eabi !OPT! %TI_COMPRESS% --symdebug:none -I"%CG%\include" --obj_directory=out\programs "%%c" %LINK% -m !B!.map -o !B!.out > !B!.build.log 2>&1
    if exist !B!.out (
      dis6x !B!.out > !B!.dis 2>&1
      ofd6x -v !B!.out > !B!.ofd 2>&1
      hex6x -q --ti_txt !B!.out -o !B!.ti.txt >nul 2>&1
      hex6x -q -i !B!.out -o !B!.intel.hex >nul 2>&1
      hex6x -q -m !B!.out -o !B!.srec >nul 2>&1
      hex6x -q -a !B!.out -o !B!.ascii.hex >nul 2>&1
      hex6x -q -t !B!.out -o !B!.tagged.hex >nul 2>&1
      call "%DSS%" "%W%\trace.js" "%CCXML%" "%W%\!B!.out" "%W%\!B!.trace" "%W%\!B!.cio" %STEPS% > !B!.dss.log 2>&1
      echo PROGRAM %%~nc.%%l build=ok>> out\programs.txt
    ) else (
      echo PROGRAM %%~nc.%%l build=FAILED>> out\programs.txt
    )
  )
)
type out\programs.txt
