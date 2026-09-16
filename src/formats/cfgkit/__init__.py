"""cfgkit — strict-typed toolchain for the game's INIT.CFG / INITLANG.CFG.

Phase 1 (this package so far): a *canonical parser* that reads the retail
UTF-16 config into a typed in-memory model mirroring the game's runtime
``dynamic::data`` arrays (defaults applied, floats float32-quantized), plus an
order-normalized semantic diff (:mod:`cfgkit.canon`).

Ground truth for every rule enforced here: the cfg grammar notes (RE'd
2026-07-05 from ``cfg::HandleConfigEntry`` & friends). The lexer
(:mod:`cfgkit.textio`) reproduces the game's space-only tokenizer; the keyword
tables (:mod:`cfgkit.grammar`) are the single source of truth for per-keyword
types and targets.
"""
