#!/usr/bin/env python3
"""tools/setup_dat.py -- read/edit the game's setup.dat net-setup MRU lists (server IP / game name /
player name), so a client can be pointed at a host WITHOUT typing the IP in the "Internet server" window.

The client reads setup.dat at boot; the FIRST entry of the server-IP MRU list pre-fills the "Server
address" field, which the direct-IP Connect uses. Writing the IP here replaces keyboard injection.

Format (RE'd from llm_cfg_save_setup_dat 0x004487a7 + llm_cfg_setup_dat_write_mru_lists 0x004bc7ea +
llm_cfg_mru_list_write 0x004bc6f0):
  [0x28 header] [LZW-compressed config block, opaque] [IP list] [GAME list] [NAME list] [trailer1 4B] [trailer2]
Each MRU list is  <count:u32> <bytelen:u32> <count NUL-terminated ASCII strings, bytelen bytes total>.
We keep the header + LZW block + GAME/NAME lists + trailers byte-for-byte and rewrite only the IP list
(and, with --name, prepend a player name to the NAME list). The lists are located by a structural parse
(no need to decode the opaque LZW block): the only offset whose IP/GAME/NAME parse reaches the trailer.

Usage:
  python tools/setup_dat.py show <setup.dat>
  python tools/setup_dat.py set  <in.dat> <out.dat> [--ip IP] [--name NAME] [--game GAME]
  python tools/setup_dat.py set-ip <in.dat> <out.dat> <ip> [--name NAME]   # legacy alias for `set --ip`

`set` PINS each provided field to a SINGLE MRU entry (so the game's field pre-fills to exactly that value)
and leaves any field not given byte-for-byte. Pinning ip+name+game makes a UI-test run deterministic and
machine-independent (the captured name/game/server text no longer depends on the machine's saved history).
"""

import argparse
import re
import struct
import sys

HEADER = 0x28
IPRE = re.compile(rb"^\d{1,3}(\.\d{1,3}){3}$")


def _parse_list(b, o):
    count, blen = struct.unpack_from("<II", b, o)
    return count, b[o + 8 : o + 8 + blen], o + 8 + blen


def _valid(count, data):
    if count > 64 or len(data) < count:
        return False
    if count == 0:
        return len(data) == 0
    if not data.endswith(b"\0"):
        return False
    parts = data.split(b"\0")[:-1]
    return len(parts) == count and all(all(32 <= c < 127 for c in p) for p in parts)


def _strings(count, data):
    return [p.decode("latin1") for p in data.split(b"\0")[:count]] if count else []


def locate(b):
    """Return (ip_off, ip, game, name) where each of ip/game/name is (count, data, end_off)."""
    for o in range(HEADER, len(b) - 8):
        try:
            ip = _parse_list(b, o)
            if ip[0] < 1 or not _valid(ip[0], ip[1]):
                continue
            if not IPRE.match(ip[1].split(b"\0")[0]):
                continue
            game = _parse_list(b, ip[2])
            if not _valid(game[0], game[1]):
                continue
            name = _parse_list(b, game[2])
            if not _valid(name[0], name[1]):
                continue
            if len(b) - name[2] >= 11:  # trailer1(4) + trailer2(>=7)
                return o, ip, game, name
        except struct.error:
            continue
    raise ValueError(
        "could not locate the MRU IP list in setup.dat (wrong file / unexpected format)"
    )


def pack_list(strings):
    data = b"".join(s.encode("latin1") + b"\0" for s in strings)
    return struct.pack("<II", len(strings), len(data)) + data


def show(path):
    b = open(path, "rb").read()
    ip_off, ip, game, name = locate(b)
    print("setup.dat %s (%d bytes); MRU section at 0x%x" % (path, len(b), ip_off))
    print("  server IPs : %s" % _strings(ip[0], ip[1]))
    print("  game names : %s" % _strings(game[0], game[1]))
    print("  player names: %s" % _strings(name[0], name[1]))


def _keep(rec):
    """Re-emit an MRU list record (count, data, end) byte-for-byte."""
    return struct.pack("<II", rec[0], len(rec[1])) + rec[1]


def _as_list(v):
    """A field value may be a single string (pin to one MRU entry) or a list of strings (pin the whole MRU
    list, first = the one the field pre-fills). Comma-separated strings are split (CLI convenience)."""
    if isinstance(v, (list, tuple)):
        return list(v)
    return [s for s in v.split(",")] if "," in v else [v]


def set_fields(inp, outp, ip=None, name=None, game=None):
    """Rewrite setup.dat pinning each of ip/name/game (when given). A value may be a single string (one MRU
    entry -- the field pre-fills to it) OR a list / comma-separated string (the whole MRU list; entry 0 is
    the pre-fill, the rest are selectable in the field's MRU dropdown -- used by the S8 dead->live IP test).
    Fields left as None keep their bytes. Header + opaque LZW block + the two trailers are byte-for-byte."""
    b = open(inp, "rb").read()
    ip_off, ip_rec, game_rec, name_rec = locate(b)
    tail = b[name_rec[2] :]  # trailer1 + trailer2, byte-for-byte
    new_ip = pack_list(_as_list(ip)) if ip is not None else _keep(ip_rec)
    new_game = pack_list(_as_list(game)) if game is not None else _keep(game_rec)
    new_name = pack_list(_as_list(name)) if name is not None else _keep(name_rec)
    out = b[:ip_off] + new_ip + new_game + new_name + tail
    open(outp, "wb").write(out)
    pins = [("IP", ip), ("name", name), ("game", game)]
    print("wrote %s: %s" % (outp, ", ".join("%s -> %s" % (k, v) for k, v in pins if v is not None)))


def set_ip(inp, outp, ip, name=None):
    """Legacy: pin the server IP (and optionally the player name). Thin wrapper over set_fields."""
    set_fields(inp, outp, ip=ip, name=name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("path")
    st = sub.add_parser("set")
    st.add_argument("inp")
    st.add_argument("outp")
    st.add_argument("--ip", default=None)
    st.add_argument("--name", default=None)
    st.add_argument("--game", default=None)
    e = sub.add_parser("set-ip")
    e.add_argument("inp")
    e.add_argument("outp")
    e.add_argument("ip")
    e.add_argument("--name", default=None)
    a = ap.parse_args()
    if a.cmd == "show":
        show(a.path)
    elif a.cmd == "set":
        if a.ip is None and a.name is None and a.game is None:
            ap.error("set: give at least one of --ip / --name / --game")
        set_fields(a.inp, a.outp, ip=a.ip, name=a.name, game=a.game)
    else:
        set_ip(a.inp, a.outp, a.ip, a.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
