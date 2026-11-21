# The LIB-REF replay fixture — what it is, and how to re-record it

Three halves that MUST come from **one run**, because separately they describe different worlds:

| half | file | what it is |
| --- | --- | --- |
| the recording | `orders.bin.zz` + `clock.bin.zz` | MHOR v2's complete native queue snapshots at both HASH and post-AI/pre-count-gate ENTRY for every step, including empty queues; raw-double clocks preserve the recorded deltas |
| the step-0 world | `world.bin.zz` | every bound region before the first simulation step, so standalone starts without a CFG parser or map reader |
| the hash stream | `hash_steps.txt.zz` + `hash_regions.txt.zz` | per-step channels and all66 per-region columns (`R <step> <h0..h65>`) |

`manifest.json` carries the provenance, the replay contract, and the fidelity measurements.
`FIDELITY.md` is generated, not written — see below.

## The faithful replay contract

**Configuration and sampling phases are part of the fixture.**

- `replay_ai_off=0`: AI runs normally. Disabling it changes more than the order queue.
- `replay_suppress_enqueue=0`: real source enqueues, overflow and loss remain active. Do not
  suppress at the entry wrapper, the sink or any other layer to make replay agree.
- MHOR v2 carries exact68-byte native records in **HASH then ENTRY order on every step**,
  including explicit empty phases. HASH replaces the queue at its existing hash sample;
  ENTRY replaces it after AI and **before** the conditional dispatch count gate.
- Only nonempty ENTRY dispatches. Handler-generated orders and canonical task direct-F execution
  run once naturally; counts or a shared prefix cannot reconstruct either phase.
- Require `HASH=5000 ENTRY=5000 missed=0 ai_off=0 suppress_enqueue=0` for these replay fixtures.
  Old or incomplete faithful streams refuse explicitly.

The generated manifest declares the stream version, both phases, native record size and effective
record/replay flags. Preserve each profile's seed, step regime and process-local gameplay/audio
configuration; an equal snapshot does not initialize unrecorded feature knobs.

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
Replay-vs-replay is bit-identical on all66 columns at every step. Record-versus-replay fidelity
also compares every nonexcluded region without sampling allowances. Existing pacing exclusions
remain explicit; `FIDELITY.md` is not permission to add gameplay masks.

## The hash stream and independent replay

The committed stream is extracted from the first replay. A second independent replay must
reproduce it exactly, and the original producer must agree on every nonexcluded region too.
MHOR v2 preserves both sampling instants, so the old single-phase periodic queue-count residue
is no longer an accepted explanation for a difference. `fixture_replay.py verify` fails closed.

## The keeper

> **"Blip vs persists" is a proxy for the transience of the STATE, not for the size of the cause.**
> One perturbation of a persistent map (an AI influence grid) is indistinguishable under that test
> from a systematic divergence, while the same perturbation of transient state (a unit's position)
> self-heals in a step and reads as benign. Both shapes here come from the SAME trigger at the SAME
> periodic instants — steps 75 and 675 — and differ only in where the mark landed. Judge a divergence
> by its mechanism and its blast radius, never by whether it healed.

## Re-recording it

The fixture becomes stale when the world schema, faithful recording format, hash manifest or
input epoch changes. The2026-11-10 QORD2 cutover genuinely recaptured all six5000-step profiles
under manifest95DC868F, world schema7EAA1059, input epoch2 and66 regions, with AI/enqueues enabled.
The producer is the shipping bound hosted execution profile, not an independent all-original
oracle. Four replay profiles have two independent hosted replay arms; two live profiles have
independent live arms. All six pass the actual standalone DLL importer/replayer with asset
absence reverified. Exact producing source/configuration/module fences are retained in
`evidence:tmp_cleanup_2026-11-15/retained/rc21_patrol/fixtures_recovery_20261110/source_receipt.json`
and `evidence:tmp_cleanup_2026-11-15/retained/rc21_patrol/fixtures_finalF_20261110/source_receipt.json`.
The approved2026-11-15 cleanup removed the old producer trees; these are selected receipts,
not a claim that whole prior trees remain. Immutable fixture manifest `run_dir`, `source_run`
and `stream_from_run` fields remain historical provenance, not live input paths.

The2026-11-08 and2026-11-04/05 cuts are historical; preserved `fidelity.re_record` retains earlier
lineage. Human-oracle migration separately requires complete freshly provisioned old/current
Native EOF comparisons:18317 tutorial and30042 campaign rows, original gameplay/clocks/excusals
preserved, and actual schema/observational-metadata differences explicitly evidenced.
Do not replace whole-tape coverage with a passing bounded window.

Current-image and metadata adjudication keepers are
`evidence:tmp_cleanup_2026-11-15/retained/rc22_followup/current_original_image_receipt.json`,
the individual `metadata_adjudication.json` files under
`evidence:tmp_cleanup_2026-11-15/retained/rc22_followup/fixture_origin_migration/runs/libref-replay-v1/`
and `libref-replay-v2/`, and the individual `campaign.json` / `tutorial.json` audits under
`evidence:tmp_cleanup_2026-11-15/retained/rc22_followup/origin_metadata_audit/`.
The preservation manifest identifies the selected oracle producer files; it does not attest
retention of every original producer-directory member or substitute receipts for recordings.

**A hash-manifest change means this procedure for every discoverable fixture and both UI-REC
oracles in the same integration, then the appropriate fingerprint-wide gate.** The blob carries
`STALE FIXTURE: captured under manifest X, current Y` and `tools/lint_fixture_currency.py` (a lint_repo row)
stays red until the stamps match the generated header (dead-ends G248). The first gate after D25 read the
stale set as three broken replays because nothing said "stale" above the fold.

```text
# 0. Provision one dedicated, idle lane using current machine/lane configuration.
#    Preserve prior fixture provenance and artifacts before replacing them.

# 1. RECORD all halves from ONE genuine run. Use the manifest's record_flags,
#    net configuration and audio fragment; keep seed_mode=2 and exit_on_stop=1.
python tools/ui_test.py sp_det.txt --harness --steps 5000 \
  --harness-extra "<record_flags>" --net-extra "<net>" \
  --extra-ini <audio-fragment> --host-dir <lane> --port <port> \
  --timeout-frames 400000 --timeout 2400 --headless

# 2. Pack actual output; use truthful producing-source provenance.
python tools/fixture_replay.py pack <record> --out <fixture> \
  --seed <seed> --commit <producing-base> --recorded <date> \
  --script sp_det.txt --host "<source/configuration receipt>" --peers "<peers>" \
  --record-flags "<record_flags JSON>" --fixed-step <0-or-1> --net "<net>"

# 3. Unpack and REPLAY #1 with the generated replay contract and SAME configuration.
#    The direct driver keeps the audio fragment identical in every arm.
python tools/fixture_replay.py unpack <fixture> --lane <lane>
python tools/ui_test.py sp_det.txt --harness --steps 5000 \
  --harness-extra "<record_flags overridden by replay_contract.flags>" \
  --net-extra "<net>" --extra-ini <audio-fragment> \
  --host-dir <lane> --port <port> --timeout-frames 400000 --timeout 2400 --headless
python tools/fixture_replay.py stream <replay1> --fixture <fixture>

# 4. Measure the ORIGINAL recording's exact two phases; no extra counts-dump run.
python tools/fixture_replay.py counts-guard <record> --fixture <fixture>

# 5. Repeat unpack + replay with a distinct fresh process, then verify.
python tools/fixture_replay.py verify <fixture> --against <record> \
  --replay-log <replay2>/mh_harness.log --write-table
python tools/fixture_replay.py check <fixture>

# 6. Exercise all discoverable fixtures in the real standalone importer/replayer.
python tools/replay_libref.py
```

For live profiles, retain `order_mode=0`, perform two independent live runs and pass both
`--live-log` arguments to `verify`; never inject an order recording. Use each fixture's actual
scenario, not `sp_det.txt` for an all-AI soak. The stock-reference audio fragment sets
`[snd] ack_cooldown_on_play=0`; the required stock gameplay knobs are declared in manifest `net`.
An `[net]` section cannot be passed through `ui_test.py --extra-ini`: use `--net-extra`.
Snapshot pre/post process directories, require successful child exit and exactly one fresh
process, then bind its session membership. A newest-directory fallback is not provenance.

**The `-v2` step regime (2026-09-30).** The same procedure records the fixed-step sets the standalone
engine replays: add `fixed_step=1` to every run's harness extras, pass `--net-extra
"sim_step_ms=20;rig_fixed_step_loop=1"` to every `ui_test.py` run, and pack with `--fixed-step 1 --net
"sim_step_ms=20;rig_fixed_step_loop=1"`. `pack` checks both against the run's `effective_config.json`,
writes `net` into the contract, and `replay` passes it back as `--net-extra` (and checks the replay run
got it). The clock track is then `0.02, 0.04, ...`, each entry the previous one plus 0.02.

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
  which keeps the mis-tagging trap above closed. Both final-step phases must still be captured;
  do not replace dispatch ENTRY with the earlier HASH snapshot.
- **`replay_ai_off=1` is the wrong flag** — see the contract above.
- **Pin `synth_seed`.** `ui_test.py` draws one per invocation (`mp_run.synth_seed()`), so record and
  replay would run different workloads. Preserve `synth_move` per profile: the movement fixture
  needs its synthetic stream; the all-AI soak deliberately uses0.
- **The game reads the recording from next to the exe**, not from the run folder
  (`harness.cpp build_paths`: `g_orders_path`/`g_clock_path` use `g_dir`). `unpack` writes there.
- **Read a red run through `mp_analyze`'s printed verdict.** A run with any environmental taint is
  re-run, never massaged.

## Retired single-phase limitations

Earlier recordings combined one sampling instant with enqueue suppression and attributed
periodic record/replay differences. Those are historical diagnostics, not the current acceptance
contract. The measured MHOR v2 cutover retains real AI/enqueues and both exact phases; it does not
accept new marks, count-prefix reconstruction or a fallback to the old faithful format.

