# Fix request for Cowork: D1–D5 from the review of 2026-10-05

This directory is a clone of `../VM6747-sim` at `43ffc5d`. Make the fixes here and leave
`../VM6747-sim` untouched.

The full review is `docs/REVIEW-2026-10-05.md`, written by Fable 5.1. Section 4 lists the
defects, each with a reproducer, and section 1 gives the analysis behind them.

## What to fix, in this order

1. **D2: store data is read in E3, not E1.** This is fatal to every program.
   Fix it at `src/C6xCpu.cpp:165` and `:267`.
   - A store's data operand must be read in E1, together with its address. Only the memory
     write happens in E3.
   - Reproducer: `STW B3,*B15--(8) || CALLP malloc,B3` in `__TI_eb_init` pushes the new B3,
     so the function returns to itself.
2. **D1: compact `MVC reg,ILC` faults.** This is fatal to every program that prints.
   The fault is at `src/C6xExec.cpp:566`.
   - rts6740's `writemsg` starts with `SPLOOPD 1 || MVC B6,ILC`, and ILC currently arrives as
     a text operand.
3. **D3: the base register's pre/post-increment lands in E3, not E1.** This gives silent wrong
   results. See `src/C6xExec.cpp:194`.
   - It is proven against TI's `hello.O2.trace`: B15 lags from TI step 467.
4. **D5: every throwing program faults in TI's unwinder.**
   - The fault is at `regb_core_get+16`: the unwinder is handed a null `_Unwind_Context`.
   - It happens on cl6x's images and on cpp11's.
5. **D4: three cl6x -O2 SPLOOP images fail.** The epilog overruns into the program and hits
   the write-conflict check.
   - The images are `divide-by-constant-signs.ti-O2`, `fill-stores.ti-O2` and
     `pointer-strides.ti-O2`.
   - Review the epilog drain and the SPMASK handling in `src/C6xLoop.cpp` against SPRUFE8
     chapter 7.

6. **D6 (new, 2026-10-06): NaN and infinity.** `runtime-shapes.cpp11-O2` runs to `C$$EXIT` and prints `0 0 -2 3`
   where the C6747 prints `1 1 -2 3`: `std::isnan(0.0 / 0.0)` and `std::isinf(1.0 / 0.0)` answer 0. It is a silent
   wrong answer. Check the DP compares (`CMPEQDP`, `CMPLTDP`, `CMPGTDP`) for NaN as unordered, and the division path
   for NaN and infinity. See `docs/TEST-2026-10-06.md`.

D1, D2 and D3 are fixed and on `master`. D4, D5 and D6 are open.

The review's scratch patches for D1–D3 are described at the end of its section 4. They got
32 of 49 images running to `C$$EXIT` with correct output.

## How the fixes will be judged

- **Every one of the 49 images runs to `C$$EXIT` with the oracle's output.** The images are in
  `~/simrev/ship` on the Linux box (`ssh ansicc`). The oracle is the CCS 5.5 C6747 simulator
  there, whose results land in `~/simrev/ship/res/`.
- **`--trace` agrees with TI's own traces in `oracle/`**, step by step, for `hello.O2` and the
  other kit programs.
- **Each fix has a test in `tests/`.** It must fail on `43ffc5d` and pass after the fix.

Please also fix the oracle kit's blind spots from section 1.6:
- the sign-extended hex in `compare-trace.py`;
- its per-cycle versus per-packet step mismatch;
- a decoder corpus that `dis6x` disassembles as instructions, not as `.word` data;
- programs linked with `rts6740_elf_eh.lib`.

Otherwise the kit will miss the next defect of this kind too.

## Rules

- The Linux box runs one DSS session at a time, and no simulator run may exceed 30 minutes.
- The Windows box (192.168.100.84) was unreachable on 2026-10-05.
- Commit here locally. Push nothing until the user says so.
