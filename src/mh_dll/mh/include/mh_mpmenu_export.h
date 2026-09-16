#pragma once
//
// MP menu restore (Workstream U) -- re-add the severed "Multiplayer" button to mh.exe's main menu.
// See the lobby RE (MP restoration, Workstream U).
//
// The whole MP flow is intact except one unwired link at the top: the main-menu button that would
// launch the network-game browser was removed. The menu is a null-terminated array of ~0x30-byte
// widget structs (pointer at container 0x00653a7f), built at runtime and set live by the menu
// activator FUN_004b5f28 (0x004b5f28). We hook that activator and, after it runs, splice in a 7th
// widget whose click callback is FUN_004bd498 (0x004bd498) -- the faithful MP entry that sets the
// network-mode flag and opens the browser (-> FUN_004bcce8 -> FUN_004bcc13 -> lobby). The new widget
// is built by CLONING a live sibling (so its runtime-built tag/flags/kind are correct); only the
// callback, item id (9, the gap in the id sequence), and geometry are overridden.
//
#ifdef __cplusplus
extern "C" {
#endif

#define MH_MENU_AUTO (-2000000000) /* geometry sentinel: derive X/Y from the cloned siblings */

// Install the FUN_004b5f28 hook (run-then-extend). Returns 1 if hooked, 0 if the prologue guard
// fails. Reads [menu] mp_x / mp_y from mh_net.ini for placement (default = auto). Call once from the
// MP arm path (MH_Seam_Init).
int MH_Menu_Install(void);

// Extend the main-menu widget array with the MP button (idempotent: a no-op once ours is installed).
// Called by the hook after FUN_004b5f28 runs; also the unit-test entry.
void MH_Menu_Apply(void);

// --- test seam (point the surgery at a mock container instead of mh.exe's 0x00653a7f) ---
void  MH_Menu_SetContainer(void **container_field);
void  MH_Menu_SetGeometry(int x, int y);
void *MH_Menu_GetWidget(void); // the built MP widget struct (or null before MH_Menu_Apply)

#ifdef __cplusplus
}
#endif
