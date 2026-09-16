# The LIB-REF replay fixture — what it is, and how to re-record it

Three halves that MUST come from **one run**, because separately they describe different worlds:

| half | file | what it is |
| --- | --- | --- |
| the recording | `orders.bin.zz` + `clock.bin.zz` | every order sitting in `order_queue` per step, tagged with its step (`mh_orders.bin`), and the per-step game clock as raw doubles (`mh_clock.bin`) so a real-time recording replays with its own deltas |
| the step-0 world | `world.bin.zz` | every bound region at the instant before the first sim step (LIB-WORLD), so a standalone host starts without a cfg parser or a map reader |
| the hash stream | `hash_steps.txt.zz` + `hash_regions.txt.zz` | LIB-REF's comparison target: the per-step `<step> <clock> <combined> <state>` lines and the per-region `R <step> <h0..h60>` lines |

`manifest.json` carries the provenance, the replay contract, and the fidelity measurements.
`FIDELITY.md` is generated, not written — see below.

## The contract LIB-REF inherits

**The replay configuration is part of the fixture, not a runner detail.** A standalone replayer must
reproduce these *semantics*, not merely read the same bytes:

- `replay_ai_off=0` — **the AI runs inside the sim.** Turning it off is not a neutral simplification:
  measured, `strat_players` / `units` / `tile_objects` diverge from around step 200, because the AI
  does more than enqueue orders and the recording only holds orders.
- `replay_suppress_enqueue=1` — the immediate-lane enqueue returns 0 and appends nothing, so the
  **injected recording is the only thing that reaches the order queue**. **The check sits at the
  SINK** (`mh::orders::detail::enqueue`), not at the game entry's promotion wrapper where it lived
  until 2026-09-11: a wrapper gate only saw callers that crossed `0x00466094`, so every
  libmh-internal caller the rebind sends straight to our body walked past it — including the
  harness's own `synth_move` workload, which appended one order per step the flag was supposed to
  have stopped. A standalone replayer must therefore suppress **at the sink too**, or it is not
  reproducing this flag (`libref_host` does, by default; `--no-suppress-enqueue` is the negative
  arm and is *expected* to diverge — measured, at step 76, the first AI enqueue). **Scope:** the
  immediate lane is the only appender in a non-lockstep session; the mode-3 lane reaches the queue
  through `release_due`, which this flag deliberately does **not** suppress.
- the injector overwrites `QUEUE[0..n]` and sets `COUNT` at the **top of each sim step**, before the
  step is hashed.

The full flag set is in `manifest.json` → `replay_contract.flags`, and `fixture_replay.py replay`
reads it from there rather than from a copy in the tool.

## Compare `state`, never `combined`

Measured at step 1 of a record and its own replay:

```
record   clock=3F9111276FB08000 combined=F452009FFCE2B0FF state=52976C9B8B49D2A8
replay   clock=3F9111276FB08000 combined=792FF0413876D562 state=52976C9B8B49D2A8
```

`state` is identical — and is the same value four independent launches produced — because it drops
the `state_excluded()` wall-clock/pacing regions. `combined` folds them in and is therefore a
property of the **run**, not of the world. LIB-REF compares `state` and the per-region `R` columns.

**And it compares them over the FULL region set: this fixture declares no exclusions.**
Replay-vs-replay is bit-identical on all 61 regions at every step, so LIB-REF's oracle needs none.
`FIDELITY.md` describes a *different* pair of runs (record vs replay) and its residue must not be
carried into LIB-REF's comparison as an exclusion.

## The hash stream is the REPLAY's, not the record's

A record run and a replay run sample `order_queue` at different instants, so a
record-stream-vs-replay comparison can never be clean however correct the fixture is — committing the
record's stream would build a permanently-red instrument artifact into LIB-REF's oracle. The
committed stream is what a **correct replayer** produces, and self-consistency is replay-vs-replay
bit-identity.

That alone would have a blind spot (a deterministically *broken* replay is also bit-identical with
itself), so the record-vs-replay comparison is part of the acceptance and is **fail-closed**: see
`FIDELITY.md` and `fixture_replay.py verify`.

## The keeper

> **"Blip vs persists" is a proxy for the transience of the STATE, not for the size of the cause.**
> One perturbation of a persistent map (an AI influence grid) is indistinguishable under that test
> from a systematic divergence, while the same perturbation of transient state (a unit's position)
> self-heals in a step and reads as benign. Both shapes here come from the SAME trigger at the SAME
> periodic instants — steps 75 and 675 — and differ only in where the mark landed. Judge a divergence
> by its mechanism and its blast radius, never by whether it healed.

## Re-recording it

The fixture rots when the blob schema (`gen_world_snapshot.py`), the recording format, or the hash
manifest moves. Every step below was executed end-to-end to produce the committed set.

```
# 0. a dedicated lane, headless
python tools/make_lane.py --name ui_libref_rec --lane 33 --port 6633 --headless

# 1. RECORD -- all three halves from ONE run. exit_on_stop=1 IS MANDATORY (see the traps).
python tools/ui_test.py sp_det.txt --harness --steps 5000 \
  --harness-extra "pin_wallclock=1;fixed_step=0;region_hash_step=1;order_mode=1;world_capture=1;\
synth_move=1;synth_seed=20260911;synth_at=60;synth_every=1;exit_on_stop=1;pin_fpu=1" \
  --host-dir <lane> --timeout-frames 400000 --timeout 2400 --headless --port 6633

# 2. pack the three halves + provenance
python tools/fixture_replay.py pack <run_dir> --seed 20260911 --commit $(git rev-parse HEAD) \
  --recorded <date> --script sp_det.txt --host "..." --peers "..." --record-flags '{...}'

# 3. REPLAY #1 from the COMMITTED form -- its stream becomes the fixture's comparison target
python tools/fixture_replay.py replay --lane <lane> --port 6633
python tools/fixture_replay.py stream <replay1_run_dir>

# 4. the 0 -> n guard: a RECORD run carrying an rdump of the order_queue_count slice over every
#    step. Its recording must be byte-identical to the committed one; counts-guard refuses otherwise.
python tools/ui_test.py sp_det.txt --harness --steps 5000 \
  --harness-extra "<the step-1 flags>;rdump_rid=42;rdump_lo=1;rdump_hi=5000" ...
python tools/fixture_replay.py counts-guard <that_run_dir> --rid 42

# 5. REPLAY #2 -- the self-consistency proof
python tools/fixture_replay.py replay --lane <lane> --port 6633

# 6. ACCEPT: integrity + the guard + replay-vs-replay bit-identity + the fail-closed fidelity table
python tools/fixture_replay.py verify --replay-log <replay2>/mh_harness.log \
  --against <record_run> --write-table
#    A NEW downstream mark refuses until somebody localizes it. `--accept-marks` seeds the entry;
#    the localization still has to be written by a human who looked (rdump the slice, diff the bytes).

# 7. the world half, against the LIB-WORLD oracle, from the committed form
python tools/fixture_replay.py unpack --lane <tmp> --world <tmp>/fixture_world.bin
src/mh_dll/Release/net_selftest.exe worldtest <tmp>/fixture_world.bin
```

## Traps, each paid for once

- **`exit_on_stop=1` is mandatory.** `on_sim_step` returns before `++g_step` once `g_active` clears at
  `stop_step`, so every order dispatched after the stop is recorded **tagged with the frozen stop
  step**. First probe: 44 records, one real and **43 mis-tagged to step 400**.
- **…and it exits ONE STEP BOUNDARY LATE, deliberately (2026-09-11).** Exiting where the stop block
  sits — `llm_strat_sim_step`'s *entry* — killed the process before the stop step's **body** ran, and
  `order_record()` hangs off `llm_strat_order_queue_dispatch`'s entry, which is inside that body. So
  the recording stopped one step short of the clock track (v5: `mh_orders.bin` max step 4999,
  `mh_clock.bin` 5000 entries), the replay injected `k=0` on the final step, and the standalone
  reference host came out one order short there. The exit is now latched and consumed at the next
  `on_sim_step` **or** `on_sim_tick` — both provably after the body, with `g_active` already false so
  nothing of step N+1 is hashed, clocked or counted. That bounds the deferral to exactly one body,
  which is what keeps the mis-tagging trap above closed. Do **not** "simplify" it by snapshotting the
  queue from the stop block instead: that is the *hash* instant, and `sim_step` runs `ai_players_tick`
  between it and dispatch entry, so the exit step would get a different sampling point from the other
  4999 — the very mechanism `attribute()` exists to account for, silently mis-stated for one row.
- **`replay_ai_off=1` is the wrong flag** — see the contract above.
- **Pin `synth_seed`.** `ui_test.py` draws one per invocation (`mp_run.synth_seed()`), so record and
  replay would run different workloads. `synth_move=0` is also wrong for a fixture: it gave 1 order
  in 400 steps (the order stream is what LIB-REF is replaying).
- **The game reads the recording from next to the exe**, not from the run folder
  (`harness.cpp build_paths`: `g_orders_path`/`g_clock_path` use `g_dir`). `unpack` writes there.
- **Read a red run through `mp_analyze`'s printed verdict.** A run with any environmental taint is
  re-run, never massaged.

## Considered and rejected

- **Recording a session with no AI order source**, so the enqueue suppression would have nothing to
  perturb. `sp_det.txt` needs a seated AI to start a match alone, and no AI-free single-peer scenario
  is proven to exist. Not attempted; recorded here so the option is not silently dropped.
- **Fixing the recording format** (two-phase record + two-phase inject) so the fixture would be
  bit-faithful to the live record as well. The design is bounded — record the pre- and post-inject
  halves separately instead of one merged stream, and replay them in the same two phases — but it
  would spend a DLL change on a measured coin-flip: a second, independent perturbation (the AI
  branching on the suppressed enqueue's return) fires at the same instants, and nothing measured
  predicts whether the fix closes the mark. LIB-REF's oracle does not need it.
