@echo off
if "%~1"==":shard" goto :shard
rem cl6x assembles every .s (asm6x itself writes COFF whatever --abi says),
rem lnk6x links each program - a Shalimar one beside its runtime - against
rem the exception-handling build of TI's runtime, the library after the objects.
set PATH=C:\ti\ccsv7\tools\compiler\ti-cgt-c6000_8.2.2\bin;%PATH%
set TILIB=C:\Users\GRA\Documents\VM6747\tilib
cd /d C:\Users\GRA\Documents\VM6747\tisweep
if not exist %TILIB%\rts6740_elf_eh.lib echo NO_EH_LIBRARY & exit /b 3
rem  Both stages sharded six ways with par.cmd, the links after every object - shm's need shmrt's.
call "%~dp0par.cmd" 6 "%~f0" asm
call "%~dp0par.cmd" 6 "%~f0" link
echo SWEEP_DONE
exit /b 0

:shard
setlocal enabledelayedexpansion
set /a I=0
goto :%~4

:asm
for %%d in (c cxx shm shmrt) do for %%f in (%%d\*.s) do (
  set /a I+=1, M=I %% %~3 + 1
  if !M!==%~2 (
    cl6x -mv6740 --abi=eabi -c %%f --output_file=%%~dpnf.obj > %%~dpnf.log 2>&1
    if errorlevel 1 echo FAILED %%f
  )
)
exit /b 0

:link
for %%d in (c cxx shm) do for %%f in (%%d\*.obj) do (
  set /a I+=1, M=I %% %~3 + 1
  if !M!==%~2 (
    set RT=
    if %%d==shm set RT=shmrt\*.obj
    lnk6x -mv6740 --abi=eabi -i %TILIB% ti-link.cmd %%f !RT! -l rts6740_elf_eh.lib -o %%~dpnf.out > %%~dpnf.lnk 2>&1
    if errorlevel 1 echo NOLINK %%f
  )
)
exit /b 0
