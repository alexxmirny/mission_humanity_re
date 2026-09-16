"use strict";
// U7 right-panel walk for a TTD replay of mh.exe (RU retail, image base 0x400000).
// Question: on the CLIENT lobby, is the map-info panel present-but-blank, hidden, or off-screen?
// Static RE (2026-07-13) showed the client's ONLY visibility diff in llm_lobby_screen_open is
// hiding the Start button (widget[1] @0x6502a3); the map-info panel is a SEPARATE static widget
// with identical flags host vs client. So dump the static right-panel widgets at runtime to see
// the real state (hidden bit / on-screen coords / content+label), and walk the live container.
//
// Static widget array: base 0x650083, stride 0x220. Known: [1]=Start btn(0x6502a3, host-only),
// [2]=OK/enter btn(0x6504c3, both). Widget layout: +4 action, +8 flags(0x80=hidden,0x40=enabled),
// +0xc draw_cb, +0x1c x, +0x20 y, +0x24 w, +0x28 h, +0x30 content ptr, +0x38 label(UTF-16 ptr).
//
// Usage (store cdb, %CDB%\cdb.exe -z <trace.run>):
//   .scriptload tools\ttd\u7_panel_dump.js
//   dx @$scriptContents.run()          // seek last widget_list_draw, dump panel + container
//   dx @$scriptContents.u7panel()      // just the static right-panel widgets
//   dx @$scriptContents.u7cont()       // just the live container child list

const CONTAINER_GLOBAL = 0x0065428d; // _G_LLM_UI_MENU_WIDGET_LIST (live container ptr)
const IS_HOST          = 0x005d55b0; // _G_LLM_NET_IS_HOST (0 = client)
const WIDGETS_BUILT    = 0x006552fa; // _G_LLM_LOBBY_WIDGETS_BUILT
const FIELD2_0x8       = 0x00654fef; // current_map_data.field2_0x8 (map player count)
const PANEL_BASE       = 0x00650083; // static lobby widget array base
const PANEL_STRIDE     = 0x220;
const PANEL_COUNT      = 9;
const SLOT_ARRAY       = 0x00653663; // where build_slot_widgets writes the slot rows
const DRAW_FN          = 0x004b6e94; // llm_ui_widget_list_draw

function log(s){ host.diagnostics.debugLog(s + "\n"); }
function u32(a){ return host.memory.readMemoryValues(a, 1, 4)[0]; }
function u8(a){ return host.memory.readMemoryValues(a, 1, 1)[0]; }
function hex(n){ return "0x" + (n >>> 0).toString(16); }
function wstr(a){
    if ((a >>> 0) === 0) return "(null)";
    try { let s = ""; for (let i = 0; i < 48; i++){ let c = host.memory.readMemoryValues(a + i*2, 1, 2)[0]; if (c === 0) break; s += String.fromCharCode(c); } return '"' + s + '"'; }
    catch (e){ return "(unreadable " + hex(a) + ")"; }
}
function drawName(cb){
    switch (cb >>> 0){
        case 0x004c0bfc: return "widget_draw";
        case 0x004c13ac: return "frame/panel";
        case 0x004b9664: return "FUN_4b9664";
        case 0x004c1611: return "spin_draw";
        case 0x00000000: return "(none)";
        default: return hex(cb);
    }
}

function pos(){ try { return host.currentThread.TTD.Position.toString(); } catch (e){ return "(live)"; } }

function dumpPanel(){
    log("=== U7 static right-panel dump @ " + pos() + " ===");
    log("IS_HOST=" + u32(IS_HOST) + "  WIDGETS_BUILT=" + u8(WIDGETS_BUILT) + "  field2_0x8(map players)=" + u32(FIELD2_0x8));
    for (let k = 0; k < PANEL_COUNT; k++){
        let b = PANEL_BASE + k*PANEL_STRIDE;
        let fl = u8(b + 8);
        let hid = (fl & 0x80) ? "HIDDEN" : "vis";
        let en  = (fl & 0x40) ? "en" : "dis";
        let x = u32(b + 0x1c) | 0, y = u32(b + 0x20) | 0, w = u32(b + 0x24) | 0, h = u32(b + 0x28) | 0;
        let tag = (b === 0x6502a3) ? "[Start]" : (b === 0x6504c3) ? "[OK]" : "";
        log("[" + k + "] @" + hex(b) + tag + " flags=" + hex(fl) + " " + hid + "/" + en +
            " draw=" + drawName(u32(b + 0xc)) + " act=" + hex(u32(b + 4)) +
            " xywh=(" + x + "," + y + "," + w + "," + h + ")" +
            " content=" + hex(u32(b + 0x30)) + " label=" + wstr(u32(b + 0x38)));
    }
    return "ok";
}

function dumpContainer(){
    let container = u32(CONTAINER_GLOBAL);
    let arr = container ? u32(container) : 0;
    log("container=" + hex(container) + "  *container(child array)=" + hex(arr) + "  (slots@" + hex(SLOT_ARRAY) + ")");
    if (!arr){ log("  (null child array = blank)"); return "blank"; }
    for (let i = 0; i < 64; i++){
        let child = u32(arr + i*4);
        if (child === 0){ log("  child count = " + i); break; }
        let fl = u8(child + 8);
        let hid = (fl & 0x80) ? "HID" : "vis";
        let x = u32(child + 0x1c) | 0, y = u32(child + 0x20) | 0, w = u32(child + 0x24) | 0, h = u32(child + 0x28) | 0;
        let inPanel = (child >= PANEL_BASE) && (child < PANEL_BASE + PANEL_COUNT*PANEL_STRIDE) && (((child - PANEL_BASE) % PANEL_STRIDE) === 0);
        let tag = inPanel ? ("[panel#" + ((child - PANEL_BASE)/PANEL_STRIDE) + "]") : "";
        log("  [" + i + "] @" + hex(child) + tag + " " + hex(fl) + " " + hid +
            " draw=" + drawName(u32(child + 0xc)) + " xywh=(" + x + "," + y + "," + w + "," + h + ")" +
            " label=" + wstr(u32(child + 0x38)));
    }
    return "ok";
}

function seekLastDraw(){
    let calls = host.currentSession.TTD.Calls(DRAW_FN);
    let last = null;
    for (let c of calls) last = c;
    if (last === null){ log("no widget_list_draw calls in trace"); return "none"; }
    last.TimeStart.SeekTo();
    log("seeked last widget_list_draw @ " + last.TimeStart.toString());
    return "ok";
}

function run(){ seekLastDraw(); dumpPanel(); dumpContainer(); return "done"; }

function initializeScript(){
    return [new host.functionAlias(run, "u7run"),
            new host.functionAlias(dumpPanel, "u7panel"),
            new host.functionAlias(dumpContainer, "u7cont"),
            new host.functionAlias(seekLastDraw, "u7seek")];
}
