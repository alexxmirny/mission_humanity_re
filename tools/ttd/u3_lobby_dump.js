"use strict";
// U3 lobby-container walk for a TTD replay of mh.exe (RU retail, image base 0x400000).
// The blank-client-lobby dig: prove whether the lobby widget rows are (a) not built,
// (b) built-but-0x80-hidden, or (c) built+visible-but-empty/off-screen.
//
// Field offsets are from the Ghidra decomp (all verified 2026-07-13):
//   llm_ui_widget_list_draw (0x004b6e94): walks 0-terminated array at *(container),
//   draws each child whose byte@+8 lacks 0x80 ("hidden") via cb ptr@+0xc.
//   widget layout: +0x08 flags, +0x0c draw_cb, +0x1c x, +0x20 y, +0x24 w, +0x28 h, +0x38 label(UTF-16 ptr).
//   build bound = current_map_data.field2_0x8 (map player count).
//
// Usage in cdb (store engine, e.g. %CDB%\cdb.exe -z <trace.run>):
//   .scriptload tools\ttd\u3_lobby_dump.js
//   dx @$scriptContents.seekLastLobbyDraw()   // positions at the last widget_list_draw
//   dx @$scriptContents.dumpLobby()           // walks the container + prints build state
// Or one-shot:  dx @$scriptContents.run()

const CONTAINER_GLOBAL = 0x0065428d; // _G_LLM_UI_MENU_WIDGET_LIST (holds the live container ptr)
const FIELD2_0x8       = 0x00654fef; // current_map_data.field2_0x8 (build bound = map player count)
const WIDGETS_BUILT    = 0x006552fa; // _G_LLM_LOBBY_WIDGETS_BUILT (build-once latch)
const SLOT_ARRAY       = 0x00653663; // &DAT_00653663 where build_slot_widgets writes the rows
const DRAW_FN          = 0x004b6e94; // llm_ui_widget_list_draw(container in EAX, __watcall)
const BUILD_FN         = 0x004bf497; // llm_lobby_build_slot_widgets()

function log(s){ host.diagnostics.debugLog(s + "\n"); }
function u32(a){ return host.memory.readMemoryValues(a, 1, 4)[0]; }
function u8(a){ return host.memory.readMemoryValues(a, 1, 1)[0]; }
function hex(n){ return "0x" + (n >>> 0).toString(16); }

function dumpContainer(container){
    log("container = " + hex(container));
    if (container === 0){ log("  (NULL container)"); return; }
    let arr = u32(container);            // *(container) = child array ptr
    log("child_array (*container) = " + hex(arr) + "   (expect " + hex(SLOT_ARRAY) + " for lobby)");
    if (arr === 0){ log("  (NULL child array -> nothing to draw = blank)"); return; }
    let i = 0;
    for (;;){
        let child = u32(arr + i*4);
        if (child === 0){ log("[" + i + "] TERMINATOR (child count = " + i + ")"); break; }
        let flags  = u8(child + 8);
        let hidden = (flags & 0x80) ? "HIDDEN" : "vis";
        let drawcb = u32(child + 0xc);
        let x = u32(child + 0x1c) | 0, y = u32(child + 0x20) | 0;
        let w = u32(child + 0x24) | 0, h = u32(child + 0x28) | 0;
        let label = u32(child + 0x38);
        log("[" + i + "] @" + hex(child) + " flags=" + hex(flags) + " " + hidden +
            " drawcb=" + hex(drawcb) + " xywh=(" + x + "," + y + "," + w + "," + h + ")" +
            " label=" + hex(label));
        if (++i > 64){ log("  (cap 64 -- array not terminated?)"); break; }
    }
}

function dumpLobby(){
    log("=== U3 lobby dump @ " + host.currentThread.TTD.Position.toString() + " ===");
    log("field2_0x8 (build bound / map player count) = " + u32(FIELD2_0x8));
    log("WIDGETS_BUILT latch = " + u8(WIDGETS_BUILT));
    dumpContainer(u32(CONTAINER_GLOBAL));
    return "ok";
}

// Seek to the last widget_list_draw whose arg (EAX) is the live menu container.
function seekLastLobbyDraw(){
    let container = u32(CONTAINER_GLOBAL);
    let calls = host.currentSession.TTD.Calls(DRAW_FN);
    let chosen = null;
    for (let c of calls){
        // EAX at call entry = first __watcall arg = the container being drawn
        if ((c.Parameters !== undefined) && false){ /* Parameters unreliable for __watcall */ }
        chosen = c; // keep last; refine below by seeking if needed
    }
    if (chosen === null){ log("no widget_list_draw calls found in trace"); return "none"; }
    chosen.TimeStart.SeekTo();
    log("seeked to last widget_list_draw @ " + chosen.TimeStart.toString() + "  EAX=" + hex(host.currentThread.Registers.User.eax));
    return "ok";
}

// Seek to build_slot_widgets and report the loop bound it saw.
function seekBuild(){
    let calls = host.currentSession.TTD.Calls(BUILD_FN);
    let n = 0, last = null;
    for (let c of calls){ n++; last = c; }
    log("build_slot_widgets call count = " + n);
    if (last === null) return "none";
    last.TimeStart.SeekTo();
    log("at build_slot_widgets entry: field2_0x8 (loop bound) = " + u32(FIELD2_0x8));
    return "ok";
}

function run(){
    seekBuild();
    seekLastLobbyDraw();
    dumpLobby();
    return "done";
}

function initializeScript(){
    return [new host.functionAlias(run, "u3run"),
            new host.functionAlias(dumpLobby, "u3dump"),
            new host.functionAlias(seekLastLobbyDraw, "u3seekdraw"),
            new host.functionAlias(seekBuild, "u3seekbuild")];
}
