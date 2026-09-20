# pacing-frametime-v1

Synthetic single-peer run directory (`host/`) for `tools/mp_pacing_report.py`'s frame-time columns
(`ft_p95`, `ft_p99`, `ft_max`, `hitch_100ms`, `hitch_250ms`). Built for **SES2b** (dead-ends G198):
`read_frametimes` used to key its whole ms conversion on a `qpc_freq=` header token D22 removed, so
every frame-time column printed `nan`/0 with exit 0 on every current log, silently.

- `host/mh_lockstep.log` -- 60 in-game (`sess=3`) rows, the current 24-column format
  (`net_lockstep.cpp:ls_log_tick`'s header), 100 ms/step. Enough rows to clear `analyse()`'s
  `len(ing) < 10` floor.
- `host/mh_frametime.log` -- 360 in-game (`game_mode=3`) rows in the **current (D22+) format**:
  header `# qpc_us game_mode`, and the `qpc_us` column is QueryPerformanceCounter ALREADY converted
  to microseconds by the DLL (net_lockstep.cpp `on_present`) -- not raw ticks, and no `qpc_freq=`
  token. ~16667 us/frame (60 fps) except row 150, which carries a deliberate **+200000 us (200 ms)
  hitch** so `hitch_100ms >= 1` and `hitch_250ms == 0` on a correct reader; a reader that still
  divides by a phantom `qpc_freq` instead prints `nan`/0 here (G198).

Regenerate: `python tools/mp_pacing_report.py tools/data/fixtures/pacing-frametime-v1/host`.
Used by `mp_pacing_report.py --selftest` and by the SES2b parser-parity check (old `read_frametimes`
vs the fixed one, same fixture -- must differ in exactly the frame-time columns).
