"""mapkit — round-trip pipeline to edit the game's binary map files in Tiled.

Formats (all reverse-engineered + byte-validated, see docs/):
  .MP           strategic planet maps   (the MP wire-format notes)      -> strategic.py
  ALIEN_*.MAP   tactical mission maps    (the mission-format notes) -> tactical.py (Phase 2)
  .TLO          biome/floor tilesets     (the TLO format notes)     -> tlo.py

Pipeline mirrors cfgkit: binary <-> Model (carries every byte) <-> Tiled JSON
(+ embedded sidecar for non-edited planes) <-> binary, proven byte-identical by
check.py / roundtrip.py / all_checks.py, installed via build_mod.py --map-src.
"""
