"""Faithful lexer for INIT.CFG / INITLANG.CFG.

Reproduces the game's ``cfg::ReadLine`` + ``ReadToSpaceOrLineEnd`` +
``GetParameterStrValue`` behaviour (see the cfg grammar notes §1):

- **UTF-16 LE**, leading BOM skipped. (The game concatenates INIT.CFG +
  INITLANG.CFG and skips only the *first* BOM; we parse each file BOM-stripped
  and separately, which is semantically identical for retail — every section in
  INIT.CFG closes with END and initlang is flat TEXT/comments — while avoiding
  the mid-buffer-BOM seam producing bogus "unknown" tokens.)
- **Lines**: CR / LF / CRLF and blank lines all terminate a line; a line never
  contains a bare CR.
- **Tokens**: only ``' '`` (U+0020 space) separates tokens. **Tabs are not
  whitespace** — a tab glues into the token (only one retail line relies on
  this: a value with trailing tabs, harmless because number parsing is
  ``sscanf``-like). Runs of spaces collapse (empty tokens are dropped).
- **Strings**: ``"…"`` runs to the closing quote or end-of-line; quotes cannot
  be escaped.
- **Numbers**: :func:`scan_int` / :func:`scan_float` mimic ``w_sscanf`` — they
  read a leading number and ignore trailing junk in the token.

Comment handling (``;`` ``#`` ``//`` ``rem``/``REM`` line comments, ``/* */``
block comments) is recognised at *dispatch position* only, so it lives in the
parser, not here; :func:`is_line_comment`, :func:`is_block_open`,
:func:`is_block_close` are the predicates it uses.
"""
from __future__ import annotations

import re
from collections import namedtuple

Token = namedtuple("Token", "is_str value")

BOM = "﻿"


def load(path: str) -> str:
    """Read a UTF-16 LE cfg file and strip a single leading BOM."""
    text = open(path, "rb").read().decode("utf-16-le")
    if text.startswith(BOM):
        text = text[1:]
    return text


def iter_lines(text: str):
    """Yield lines the way ``ReadLine`` splits them (CR/LF/CRLF, blanks kept)."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text.split("\n")


def tokenize(line: str):
    """Tokenize one line into :class:`Token`\\ s (space-only separation)."""
    toks = []
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c == " ":
            i += 1
            continue
        if c == '"':
            j = line.find('"', i + 1)
            if j == -1:  # unterminated quote runs to end of line
                toks.append(Token(True, line[i + 1:]))
                break
            toks.append(Token(True, line[i + 1:j]))
            i = j + 1
            continue
        j = i
        while j < n and line[j] != " ":  # tabs glue; only space breaks a token
            j += 1
        toks.append(Token(False, line[i:j]))
        i = j
    return toks


_INT_RE = re.compile(r"\s*([+-]?\d+)")
_FLOAT_RE = re.compile(r"\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)")


def scan_int(s: str):
    """``sscanf("%d")``: leading (optionally signed) integer, trailing junk ok.

    Returns ``(value, ok)``.
    """
    m = _INT_RE.match(s)
    return (int(m.group(1)), True) if m else (0, False)


def scan_float(s: str):
    """``sscanf("%f")``: leading real number, trailing junk ok. ``(value, ok)``."""
    m = _FLOAT_RE.match(s)
    return (float(m.group(1)), True) if m else (0.0, False)


# ---- comment predicates (used by the parser at dispatch position) ----

def is_line_comment(tok: Token) -> bool:
    if tok.is_str:
        return False
    v = tok.value
    return v[:2] == "//" or (v[:1] in (";", "#")) or v in ("rem", "REM")


def is_block_open(tok: Token) -> bool:
    return not tok.is_str and tok.value[:2] == "/*"


def is_block_close(tok: Token) -> bool:
    return not tok.is_str and tok.value[:2] == "*/"
