"use strict";
// GAME_OPTIONS (+ low-confidence container) reachability trace for a TTD replay of mh.exe.
// (RU retail, image base 0x400000.)
//
// WHY: three containers named 2026-07-14 have NO static builder xref because they are installed
// into _G_LLM_UI_MENU_WIDGET_LIST (0x65428d) via a COMPUTED container-table index
// (base 0x653a7f = MAIN_MENU = index 0, stride 0x4c). We can't tell statically whether they are
// reachable or dead code. This script answers it from a trace: enumerate every WRITE to the
// container-list global and report which container each write installs -- proving whether
// GAME_OPTIONS (index 18) / the two tutorial containers are ever reached, and by whom.
//
// TARGETS (container VA -> name), the ones we're unsure about:
//   0x653fd7 GAME_OPTIONS (idx 18)   0x653acb TUTORIAL_INTRO (idx 1)   0x653b63 TUTORIAL_DLG (idx 3)
// GAME_OPTIONS's one action widget 0x6511c7 -> FUN_004c7ac2 (applies per-player relations/control);
// bp 0x4c7ac2 in a LIVE run is the cheap confirm that the screen is actually used.
//
// HOW TO PRODUCE THE TRACE (drive the game through everything menu/options/tutorial-ish):
//   %TTD%\TTD.exe -accepteula -out <dir> -maxFile 2048 -launch F:\games\MH\mh.focus.exe
//   ... click through: New Game -> race -> any options/setup screens; start + quit a skirmish;
//       run the Tutorial from the main menu; open the in-game menu; trigger a win/lose ...
//   %TTD%\TTD.exe -stop all
// Keep it as short as still exercises those paths (indexing cost scales with instructions).
//
// REPLAY (store cdb):
//   set _NT_SYMBOL_PATH=
//   %CDB%\cdb.exe -z <trace>.run -c ".scriptload tools\ttd\game_options_trace.js; dx @$scriptContents.run(); qq"
//   dx @$scriptContents.run()      // histogram of every container installed + flag the 3 unknowns
//   dx @$scriptContents.seek(0x653fd7)   // seek to the first write that installs GAME_OPTIONS, then `k`
//
// LIVE alternative (no TTD; README notes live-attach beats a >2GB full trace): under cdb,
//   ba w4 0x65428d ".if (poi(0x65428d)==0x653fd7) {.echo GAME_OPTIONS reached; k} .else {gc}"
//   then play through the menus; a hit + stack = the reacher. No hit after thorough play = dead.

const CONTAINER_GLOBAL = 0x0065428d; // _G_LLM_UI_MENU_WIDGET_LIST
const TBL_BASE = 0x00653a7f;         // container table index 0 (MAIN_MENU), stride 0x4c
const TBL_STRIDE = 0x4c;
const NAMES = {                      // VA -> friendly name (subset; extend as needed)
    0x00653a7f: "MAIN_MENU", 0x00653acb: "TUTORIAL_INTRO*", 0x00653b17: "TUTORIAL_STEP",
    0x00653b63: "TUTORIAL_DLG*", 0x00653baf: "TUTORIAL_MENU", 0x00653bfb: "SAVEGAME_LIST",
    0x00653c47: "RACE_SELECT", 0x00653c93: "SHIPMENT_CONTENTS", 0x00653cdf: "PLANET_SEL",
    0x00653d2b: "BUILD_MENU_LIST", 0x00653d77: "ENTER_GAMEPLAY", 0x00653dc3: "OUTCOME",
    0x00653e0f: "TEXT_VIEWER", 0x00653e5b: "MP_LOCAL_BROWSER", 0x00653ea7: "MP_SESSION_BROWSER",
    0x00653ef3: "NET_SETUP", 0x00653f3f: "LOBBY", 0x00653f8b: "MP_MAP_PICKER",
    0x00653fd7: "GAME_OPTIONS*", 0x00654023: "LOCKSTEP_OVERLAY", 0x0065406f: "MENU_OVERLAY",
    0x0065fa9f: "MESSAGE", 0x006540bb: "DLG_SCRATCH_LIST"
};
const UNKNOWNS = { 0x00653fd7: "GAME_OPTIONS", 0x00653acb: "TUTORIAL_INTRO", 0x00653b63: "TUTORIAL_DLG" };

function log(s){ host.diagnostics.debugLog(s + "\n"); }
function hex(n){ return "0x" + (n >>> 0).toString(16); }
function nameOf(v){
    v = v >>> 0;
    if (NAMES[v]) return NAMES[v];
    if (v >= TBL_BASE && v < TBL_BASE + 40*TBL_STRIDE && ((v - TBL_BASE) % TBL_STRIDE) === 0)
        return "container[" + ((v - TBL_BASE)/TBL_STRIDE) + "]";
    if (v === 0) return "(cleared 0)";
    return hex(v);
}

// enumerate all writes to the container-list global across the trace
function writes(){
    return host.currentProcess.TTD.Memory(CONTAINER_GLOBAL, CONTAINER_GLOBAL + 4, "w");
}

function run(){
    log("=== container-list installs (writes to _G_LLM_UI_MENU_WIDGET_LIST @0x65428d) ===");
    let hist = {}, order = [], hitUnknown = {};
    for (let a of writes()){
        let v = 0; try { v = a.Value >>> 0; } catch (e) { continue; }
        let nm = nameOf(v);
        if (hist[nm] === undefined){ hist[nm] = 0; order.push(nm); }
        hist[nm]++;
        if (UNKNOWNS[v]){
            hitUnknown[v] = (hitUnknown[v]||0) + 1;
            let t = ""; try { t = a.TimeStart.toString(); } catch(e){}
            let ip = ""; try { ip = hex(a.IP); } catch(e){}
            log("  >>> " + UNKNOWNS[v] + " installed @time " + t + " ip=" + ip);
        }
    }
    log("--- histogram (value installed x count) ---");
    for (let nm of order) log("  " + nm + " : " + hist[nm]);
    log("--- verdict for the 3 unknowns ---");
    for (let v in UNKNOWNS)
        log("  " + UNKNOWNS[v] + " (" + v + "): " + (hitUnknown[v] ? (hitUnknown[v] + " install(s) -> REACHABLE") : "NEVER installed -> likely DEAD/vestigial"));
    return "done";
}

// seek to the first write that installs container VA `target`; then run `k` in cdb for the stack
function seek(target){
    target = target >>> 0;
    for (let a of writes()){
        let v = 0; try { v = a.Value >>> 0; } catch (e){ continue; }
        if (v === target){ a.TimeStart.SeekTo(); log("seeked install of " + nameOf(target) + " @ " + a.TimeStart.toString() + " -- now run `k`"); return "ok"; }
    }
    log(nameOf(target) + " never installed in this trace"); return "none";
}

function initializeScript(){
    return [new host.functionAlias(run, "goRun"),
            new host.functionAlias(seek, "goSeek")];
}
