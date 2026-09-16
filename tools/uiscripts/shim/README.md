# Shim timelines

`<ms> <command>` lines for `tools/net_shim.py --timeline`. Blank lines and `#` comments are ignored;
steps are sorted, so order in the file does not matter.

**Time zero is the FIRST connection through the shim, not process start** — so the operator's launch
latency is not part of the experiment and two runs of the same timeline are comparable. In a UI-path
run that first connection is the client's discovery connect, which happens in the lobby, *before*
Start. Budget for the lobby + map handoff (~60-90 s on the rig) before the peers are actually in a
match and any in-game measurement means anything.

The offsets below are first estimates from the rig's observed pacing. Check the shim log against the
peers' `mh_net.log` on the first run and retune rather than assuming they lined up.

| File | For |
| --- | --- |
| `p1_latency.txt` | P1's last `done_when`: a mid-run latency change the adaptive controller must grow into and shrink back from |
| `rlive_link_death.txt` | R-live end-to-end in the real game, including the negative control (quiet ≠ dead) |
