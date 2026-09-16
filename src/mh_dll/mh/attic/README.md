# attic — dormant experiments, excluded from the build

- `winmain_experiment.c` + `addresses.h` + `mh.h` — the from-scratch WinMain / EXTERMIN.EXE
  experiment (EXTERM_* VAs are a *different binary's* addresses, not mh.exe). Not referenced by
  any live code path; parked here during the 2026-07 mh_lib refactor instead of
  deleted, in case the Extermination side is picked up
  again. Nothing in the solution compiles these files.
