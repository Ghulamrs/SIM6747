# oracle — vm6747 held to TI's CCS 5.5 simulator

vm6747's machine-code path (loading a cl6x `.out`, `.bin` or hex image and running TI's own rts6740)
is built clean-room from TI's public documents and checked here, black-box, against TI's own tools
on the two boxes. Nothing of TI's is disassembled or copied; its tools are only run.

    oracle/run.sh linux|windows|both [steps]      # from your own Terminal on the Mac

| box | reached by | CCS 5.5 |
|---|---|---|
| linux | `ssh -i ~/Documents/_NEW_/myMorningWalk.pem ec2-user@52.202.164.123` | `~/ti/ccsv5` |
| windows | `ssh GRA@192.168.100.84` (PowerShell; each step handed to `cmd /c`) | `C:\ti\ccsv5` |

What comes back, in `oracle/results/<box>/`:

- `inventory.txt` and `targetdb/` — the compiler and simulator configurations that CCS holds, and its documents.
- `words.dis` — what dis6x makes of `words.asm`, 4096 fetch packets of seeded pseudo-random words
  (`gen-words.py`), about a third of them with a compact-instruction header. The decoder must
  match this word for word.
- `programs/<name>.<O2|O0>.*` — for each `programs/*.c` built by cl6x 7.4.4 and linked with
  `rts6740_elf.lib`:
  - the `.out` and its `.map`;
  - `dis6x` and `ofd6x -v` listings;
  - `hex6x` images (`--ti_txt`, Intel, Motorola-S, ASCII-hex, TI-tagged);
  - `.trace`, written by `remote/trace.js`. It steps the C6747 cycle-accurate simulator one
    execute packet at a time from the load entry, and records PC, A0–B31, the control registers
    and the CPU cycle count after each step;
  - `.cio`, the program's output.

`intrin.c` and `intrin2.c` run one C6000 intrinsic per line, so each SIMD, saturating, bit-field,
multiply and floating-point instruction has its result on record. `intrin2.c` holds the intrinsics
whose cl6x 7.4.4 spelling is less certain; if one of them fails to build, only that program is lost.
