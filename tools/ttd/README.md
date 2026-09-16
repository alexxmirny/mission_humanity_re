# TTD capture + replay for the U3 blank-client-lobby dig

Time Travel Debugging harness for `mh.exe`. Records the **client** reaching the (blank) real
lobby, then walks the lobby widget container offline to prove whether the rows are not-built /
built-but-hidden / built-but-empty. Open MP item U3 carries the reframed hypothesis.

## Tools (copied out of the WinDbg Store pkg — the Store folders are ACL-locked in place)

Two placeholders below, because these are machine-local installs with no committed default
(fork F5B — they used to be one operator's own absolute paths): **`%TTD%`** = where you unpacked
the x86 TTD recorder, **`%CDB%`** = where you unpacked the x86 store WinDbg. **Run `cdb` from the
repo root** — every path into this repo below (`-c "$$><tools\ttd\…"`, `.scriptload tools\ttd\…`)
is resolved against cdb's working directory, which is what makes them portable *and* still
executable; `walk_forward.txt`'s `.scriptload` is a real command, not a comment.

- **Record:** `%TTD%\TTD.exe`  (TTD 1.01.11 x86; EULA already accepted)
- **Replay:** `%CDB%\cdb.exe`  (store engine — the SDK's cdb lacks TTD `.run` support)
- **Walk script:** `tools/ttd/u3_lobby_dump.js`

## Rig (no role-flip — reuse the WORKING host config)
- **HOST = dev box .61**, `F:\games\MH`, `mh_net.ini role=host` (as configured). Launch the host
  normally (GUI): `mh.focus.exe --mp-host-lobby`.
- **CLIENT = VM .37 under TTD**, `C:\games\mh`, `mh_net.ini host=192.168.0.61`. The scriptable
  `--mp-join-lobby` verb drives it into the SAME real lobby (join handler FUN_004bddff) with no
  menu clicking. Deploy TTD to the VM (`C:\ttd\`) and launch via `schtasks /it` (SSH can't render
  DirectDraw).

Single-box 2-peer is impossible (two DirectDraw instances). TTD adds ~4x overhead but DirectDraw
renders fine (verified on the dev box menu). The lobby is pre-Start (no tight lockstep) so the
slowdown is tolerable.

## Record (on the client)
Use a **FULL** trace (not `-ring`) so the one-shot `build_slot_widgets` call is always retained,
then stop ~8 s after the lobby appears (keep it SHORT — indexing cost scales with recorded
instructions; the 512 MB menu-idle test took minutes to index):

    C:\ttd\TTD.exe -accepteula -out C:\games\mh\ttd -maxFile 1024 -launch C:\games\mh\mh.exe --mp-join-lobby
    ...  (let the lobby render ~8 s)  ...
    C:\ttd\TTD.exe -stop all

Pull the `.run` back to the dev box (scp) for replay.

## Replay-walk (on the dev box)
    set _NT_SYMBOL_PATH=
    %CDB%\cdb.exe -z <trace>.run -c ".scriptload tools\ttd\u3_lobby_dump.js; dx @$scriptContents.run(); qq"

`run()` = seek build_slot_widgets (report the loop bound `field2_0x8`) -> seek the last
widget_list_draw -> walk the container children (per child: `+8` flags incl. `0x80` hidden,
`+0xc` draw cb, `+0x1c/+0x20` x/y). Compare against a host-side capture of the same.

## Gotchas learned
- `.run` will NOT open with the SDK cdb (`-z` -> "invalid dump signature"); use the store cdb.
- A `-ring` trace only knows memory WRITTEN in the retained window -> read values at a code point
  that touches them (the script anchors on build/draw calls), or use a full trace.
- First `TTD.Calls()` / reverse-exec on a fresh trace builds the index (slow, one-time; `.idx`
  written next to the `.run` and reused). Keep captures short.
