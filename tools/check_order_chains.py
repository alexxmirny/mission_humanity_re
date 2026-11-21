#!/usr/bin/env python3
"""Judge real order-chain consumers in explicitly named process/session recordings.

No live runner, newest-directory lookup,
fixture generation, replay suppression, or private verdict masks live here.
"""

import argparse
import gzip
import json
import math
import re
import struct
from pathlib import Path

import _rundir
import mh_order_stream as orders
import state_record as sr

LAYOUTS = {1: (96, 192), 2: (128, 240)}
NODE, SLOTS, NO_CYCLE, CHUNK = 48, 32, 0xFFFFFFFF, 64
# Successors EN0066a224; physical deltas EN00ae2d08 (initializer0045fac0).
DIRECTIONS = (
    (0, -1, 0, 16, 8),
    (1, 0, 1, 17, 9),
    (0, 1, 2, 18, 10),
    (-1, 0, 3, 19, 11),
    (1, -1, 4, 20, 13),
    (1, 1, 5, 21, 14),
    (-1, 1, 6, 22, 15),
    (-1, -1, 7, 23, 12),
    (0, -1, 4, 20, 13),
    (1, 0, 5, 21, 14),
    (0, 1, 6, 22, 15),
    (-1, 0, 7, 23, 12),
    (-1, -1, 0, 16, 8),
    (1, -1, 1, 17, 9),
    (1, 1, 2, 18, 10),
    (-1, 1, 3, 19, 11),
    (0, -1, 7, 23, 12),
    (1, 0, 4, 20, 13),
    (0, 1, 5, 21, 14),
    (-1, 0, 6, 22, 15),
    (1, -1, 0, 16, 8),
    (1, 1, 1, 17, 9),
    (-1, 1, 2, 18, 10),
    (-1, -1, 3, 19, 11),
)
APPLIED, EXCURSION, RETURN_MODE = 16, 4, 8


def require(ok, message):
    if not ok:
        raise ValueError(message)


def pick(directory, stem):
    paths = [p for p in (directory / stem, directory / (stem + ".gz")) if p.is_file()]
    require(len(paths) == 1, "expected exactly one raw/gzip artifact: " + str(directory / stem))
    return paths[0]


def raw(path):
    data = path.read_bytes()
    return gzip.decompress(data) if path.suffix == ".gz" else data


def session_for(target, selected=None):
    """An explicit session stays that session; a multi-session boot needs a selector."""
    target = Path(target).resolve()
    members = [Path(p).resolve() for p in _rundir.process_and_sessions(str(target))]
    process, sessions = members[0], members[1:]
    if _rundir.is_session_dir(str(target)):
        require(selected is None, "do not combine an explicit session target and --session")
        session = target
    elif selected is not None:
        candidate = Path(selected)
        session = (
            process.parent / candidate if candidate.parent == Path(".") else candidate
        ).resolve()
    else:
        require(len(sessions) == 1, "multiple sessions: supply an explicit session directory")
        session = sessions[0]
    require(session in sessions, "session is not a member of this explicit process")
    metadata = _rundir.read_session_json(str(session))
    require(
        metadata and metadata.get("process_dir") == process.name, "session/process binding mismatch"
    )
    drive = (process / "mh_uidrive.log").read_text(encoding="utf-8", errors="replace")
    require(
        "[script] COMPLETE" in drive and "[script] ABORT" not in drive,
        "actual UI script did not complete",
    )
    return process, session, metadata


def frames(rec):
    """One final state per step, using state_record's actual chunk types, not state_at N times."""
    buf, pending, yielded = None, None, None
    for chunk in rec.chunks:
        if not isinstance(chunk, (sr.KeyframeChunk, sr.StepChunk)):
            continue
        if pending is not None and chunk.step != pending:
            require(chunk.step == pending + 1, "noncontiguous state-record step axis")
            yield pending, buf
            yielded = pending
        if isinstance(chunk, sr.KeyframeChunk):
            buf = bytearray(chunk.state)
        else:
            require(buf is not None, "state delta precedes initial keyframe")
            for rid, offset, length, data in chunk.runs:
                start = rec.region_offset[rid] + offset
                buf[start : start + length] = data
        pending = chunk.step
    require(
        pending is not None and (yielded is None or pending == yielded + 1),
        "empty/incomplete step axis",
    )
    yield pending, buf


class Evidence:
    def __init__(self, target, selected=None):
        self.process, self.session, self.metadata = session_for(target, selected)
        harness = (self.process / "mh_harness.log").read_text(encoding="utf-8", errors="replace")
        armed = re.findall(r"; ==== mh replay harness armed: ([^\n]+)", harness)
        require(len(armed) == 1, "missing/ambiguous actual harness arming receipt")
        settings = dict(re.findall(r"(\w+)=(-?\d+)", armed[0]))
        require(
            settings.get("replay_ai_off") == "0"
            and settings.get("suppress_enqueue") == "0"
            and settings.get("seed_mode") in ("0", "2")
            and settings.get("order_mode") in ("0", "1"),
            "live workload used AI/enqueue suppression or injected replay/seed state",
        )
        require(
            re.search(r"; synth_move=0 armed=0\b", harness),
            "synthetic movement contaminated the actual-input workload",
        )
        self.rec = sr.load(str(pick(self.session, "mh_match_state.bin")))
        r = self.rec
        require(r.stop_reason is None and r.end is not None, "state record is damaged or lacks END")
        require(
            not r.header.is_ring and r.first_step == 1 and r.last_step > 1,
            "expected complete non-ring session record",
        )
        require(
            r.end.last_step == r.last_step and r.end.steps_recorded == r.last_step,
            "state END coverage mismatch",
        )
        require(
            sum(isinstance(c, sr.EndChunk) for c in r.chunks) == 1
            and isinstance(r.chunks[-1], sr.EndChunk),
            "state END is not unique/final",
        )
        prefix = "mh_match" if settings["order_mode"] == "1" else "mh_rec"
        self.samples = orders.load(
            pick(self.session, prefix + "_orders.bin"), first_step=1, steps=r.last_step
        )
        self.clocks = raw(pick(self.session, prefix + "_clock.bin"))
        require(
            len(self.clocks) == 8 * r.last_step, "clock track does not cover every state/MHOR step"
        )
        self.bases = dict(zip((d.name for d in r.regions), r.region_offset))
        self.lengths = {d.name: d.len for d in r.regions}
        for name in ("order_tasks", "units", "buildings", "storage_policy", "game_clock"):
            require(name in self.bases, "missing consumer region " + name)
        require(self.lengths["game_clock"] == 8, "unexpected game_clock width")
        self.layouts = sr._get_structs()
        self.fields = {}
        for region in ("units", "buildings"):
            layout = self.layouts[sr.REGION_LAYOUTS[region]["struct"]]
            require(layout.reliable, "unreliable generated record layout: " + region)
            self.fields[region] = {f.name: f for f in layout.fields}
        self.shape = None

    def region(self, buf, name):
        at = self.bases[name]
        return memoryview(buf)[at : at + self.lengths[name]]

    def field(self, buf, region, player, slot, name, fmt=None):
        layout = self.layouts[sr.REGION_LAYOUTS[region]["struct"]]
        players = self.shape["players"]
        length = self.lengths[region]
        require(length % (players * layout.size) == 0, "invalid native record pitch: " + region)
        pitch = length // (players * layout.size)
        require(
            0 <= player < players and 0 < slot < pitch,
            "native actor/target outside recorded capacity",
        )
        f = self.fields[region][name]
        at = self.bases[region] + (player * pitch + slot) * layout.size + f.offset
        if fmt:
            require(struct.calcsize(fmt) == f.size, "generated field width changed: " + name)
            return struct.unpack_from(fmt, buf, at)[0]
        return int.from_bytes(buf[at : at + f.size], "little")

    def unit(self, buf, player, slot):
        names = (
            "unit_proto_id",
            "state",
            "order",
            "x",
            "y",
            "goal_x",
            "goal_y",
            "move_microstep",
            "move_heading",
            "path_cursor",
            "path_slot_id",
            "target_ref",
            "target_index",
            "committed_weapon_damage_est",
            "order_status_flags",
        )
        out = {n: self.field(buf, "units", player, slot, n) for n in names}
        out["hp"] = self.field(buf, "units", player, slot, "energy", "<d")
        out["activity_clock"] = self.field(buf, "units", player, slot, "activity_clock", "<d")
        out["game_clock"] = struct.unpack("<d", self.region(buf, "game_clock"))[0]
        out["xy"] = [out["x"], out["y"]]
        # The raised carrier mirrors min(slot,254) in the old byte; the full
        # uint16 at0xda is authoritative for those escaped IDs (path_pool.cpp).
        # MHSR stores raw unit bytes even when verdict masks exclude them
        # (inc_state.h24..28). Never derive a full slot from canonical QORD.
        slot_id = out["path_slot_id"]
        if slot_id == 254:
            layout = self.layouts[sr.REGION_LAYOUTS["units"]["struct"]]
            pitch = self.lengths["units"] // (self.shape["players"] * layout.size)
            at = self.bases["units"] + (player * pitch + slot) * layout.size
            require(
                layout.size >= 0xDC and at + 0xDC <= self.bases["units"] + self.lengths["units"],
                "missing physical extended path-slot bytes in recorded unit",
            )
            slot_id = struct.unpack_from("<H", buf, at + 0xDA)[0]
            require(254 <= slot_id < 500, "invalid raised native path-slot escape")
        out["native_slot"] = NO_CYCLE if out["path_slot_id"] == 255 else slot_id
        return out

    def identity(self, buf, kind, player, slot):
        policy = self.region(buf, "storage_policy")
        require(len(policy) >= 32, "short lifetime-policy header")
        magic, schema, players, buildings, units, width, height, size = struct.unpack_from(
            "<8I", policy
        )
        require(
            magic == 0x31525053
            and schema == 1
            and size == len(policy) == 32 + players * (buildings + units) * 16,
            "unexpected lifetime-policy layout",
        )
        require(
            players == self.shape["players"]
            and units == self.shape["units"]
            and buildings == self.shape["buildings"]
            and (width, height) == (self.shape["width"], self.shape["height"]),
            "canonical/policy dimensions disagree",
        )
        capacity = units if kind == 1 else buildings
        require(
            kind in (1, 2) and 0 <= player < players and 0 < slot < capacity,
            "lifetime identity outside recorded policy",
        )
        at = 32 + (players * buildings * 16 if kind == 1 else 0) + (player * capacity + slot) * 16
        return struct.unpack_from("<2I", policy, at)

    def target_hp(self, buf, target):
        kind, player, slot, life = target
        require(kind in (1, 2) and life > 0, "invalid expected target identity")
        actual_life, flags = self.identity(buf, kind, player, slot)
        require(actual_life == life, "target slot was reused by a different lifetime")
        region = "units" if kind == 1 else "buildings"
        hp = self.field(buf, region, player, slot, "energy", "<d")
        require(hp <= 0 or flags & 1, "positive-HP target has no matching live canonical identity")
        return hp

    def row(self, buf, actor):
        region = self.region(buf, "order_tasks")
        require(len(region) >= 8, "short canonical header")
        magic, schema = struct.unpack_from("<2I", region)
        require(magic == 0x31524F51 and schema in LAYOUTS, "unsupported canonical magic/schema")
        header, prefix = LAYOUTS[schema]
        row_bytes = prefix + NODE * SLOTS
        require(len(region) >= header, "short canonical schema header")
        h = struct.unpack_from(f"<{header // 4}I", region)
        players, units, width, height = h[2:6]
        require(
            0 < players <= 8
            and 0 < units <= 65535
            and 0 < h[18] <= 65535
            and 0 < width <= 256
            and 0 < height <= 256
            and h[7] < 32
            and h[8] > 0,
            "invalid canonical dimensions/world",
        )
        require(
            h[9] == len(region) == header + players * units * row_bytes,
            "canonical region length mismatch",
        )
        require(not any(h[10 + players : 18]), "unused command highwater is nonzero")
        frontier = list(h[19:27]) if schema == 2 else [0] * 8
        require(not any(h[27:] if schema == 2 else h[19:]), "canonical reserved header changed")
        require(
            not any(frontier[players:]) and all(frontier[p] <= h[10 + p] for p in range(players)),
            "invalid canonical frozen-root frontier",
        )
        shape = {
            "players": players,
            "units": units,
            "width": width,
            "height": height,
            "world": list(h[6:9]),
            "buildings": h[18],
        }
        self.shape = shape
        p, slot, life = actor
        require(
            0 <= p < players and 0 < slot < units and life > 0,
            "required actor outside canonical dimensions",
        )
        start = header + (p * units + slot) * row_bytes
        words = struct.unpack_from(f"<{prefix // 4}I", region, start)
        q = {
            name: words[offset // 4]
            for name, offset in (
                ("life", 0),
                ("route", 4),
                ("next_leg", 8),
                ("attempt", 12),
                ("phase", 16),
                ("failure", 20),
                ("count", 24),
                ("cycle", 28),
                ("cursor", 32),
                ("placement", 44),
                ("valid", 48),
                ("path", 52),
                ("flags", 64),
                ("circuit_cursor", 76),
                ("progress", 88),
                ("head", 92),
                ("family", 96),
                ("tracking", 100),
                ("epoch", 104),
                ("ex_objective", 124),
            )
        }
        q.update(schema=schema, highwater=list(h[10:18]), frontier=frontier)
        q["assigned"], q["anchor"] = list(words[9:11]), list(words[17:19])
        q["implicit"], q["circuit_start"] = list(words[14:16]), list(words[20:22])
        q["ex_target"], q["reservation"] = list(words[27:31]), list(words[32:44])
        extension = list(words[44:60]) if schema == 2 else [0] * 16
        if schema == 1:
            require(not any(words[44:]), "legacy canonical reserved row changed")
        q["origin"], q["join_start"] = extension[:2], extension[4:6]
        for name, index in (
            ("origin_state", 2),
            ("air_mode", 3),
            ("join_heading", 6),
            ("join_phase", 7),
            ("join_progress", 8),
            ("curve_index", 9),
            ("chunk_ack", 10),
            ("chunk_revision", 11),
            ("spec_revision", 12),
            ("air_slot_plus1", 13),
            ("return_heading", 14),
            ("air_spec_count", 15),
        ):
            q[name] = extension[index]
        q["ticket_owner"] = p
        require(
            q["count"] <= SLOTS and q["head"] < SLOTS and q["phase"] <= 7,
            "invalid canonical row bounds",
        )
        ring = [
            list(struct.unpack_from("<12I", region, start + prefix + i * NODE))
            for i in range(SLOTS)
        ]
        q["nodes"] = [ring[(q["head"] + i) % SLOTS] for i in range(q["count"])]
        require(
            all(not any(ring[(q["head"] + i) % SLOTS]) for i in range(q["count"], SLOTS)),
            "nonzero unused canonical ring node",
        )
        validate_row(q, words, shape, life)
        if schema == 2 and q["count"] and q["cycle"] != NO_CYCLE and q["origin_state"] in (0, 1):
            root = q["nodes"][q["cycle"]][8]
            if root:
                for other_slot in range(units):
                    at = header + (p * units + other_slot) * row_bytes
                    other_count, other_cycle = struct.unpack_from("<2I", region, at + 24)
                    if not other_count or other_cycle == NO_CYCLE:
                        continue
                    require(
                        other_count <= SLOTS and other_cycle < other_count,
                        "invalid cohort peer ring bounds",
                    )
                    other_head = struct.unpack_from("<I", region, at + 92)[0]
                    require(other_head < SLOTS, "invalid cohort peer ring head")
                    other_origin = list(struct.unpack_from("<3I", region, at + 176))
                    other_root = struct.unpack_from(
                        "<I", region, at + prefix + ((other_head + other_cycle) % SLOTS) * NODE + 32
                    )[0]
                    if other_root == root and other_origin[2] in (0, 1):
                        require(
                            other_origin == q["origin"] + [q["origin_state"]],
                            "same command-root cohort has inconsistent frozen origin",
                        )
        q["active"] = q["nodes"][q["cursor"]] if q["count"] else None
        q["ticket"] = tuple(
            shape["world"]
            + list(actor)
            + [q["route"], q["active"][9] if q["active"] else 0, q["attempt"]]
        )
        return q


def signature(node):
    return node[:7]


def validate_row(q, words, shape, life):
    """Schema-dispatched structural checks; guided rows keep the finite grammar."""

    def tile(xy):
        return xy[0] < shape["width"] and xy[1] < shape["height"]

    def target(t):
        kind, owner, slot, incarnation = t
        return (
            not any(t)
            if kind == 0
            else kind in (1, 2)
            and owner < shape["players"]
            and incarnation > 0
            and slot < shape["units" if kind == 1 else "buildings"]
        )

    r = q["reservation"]
    require(
        r[0] <= 3
        and r[5] <= 65535
        and not r[10] & ~0x8001FFFF
        and target(r[1:5])
        and target(r[6:10]),
        "invalid canonical primary reservation",
    )
    if not r[0]:
        require(not any(r), "nonempty unbound primary reservation")
    elif r[0] == 2:
        require(
            r[1] != 0 and not any(r[6:10]) and r[10] & 65535 == r[5] and r[11] <= q["epoch"],
            "invalid bound primary reservation",
        )
    else:
        require(
            r[0] == 1 and r[1] != 0 and r[11] == q["epoch"] > 0,
            "invalid pending or checkpoint handoff reservation",
        )
    if not q["life"]:
        require(not any(words) and not q["nodes"], "nonzero unowned canonical row")
        return
    count, flags, cycle, cursor = (q[k] for k in ("count", "flags", "cycle", "cursor"))
    if not count:
        require(
            (q["next_leg"] > 0 and q["epoch"] >= q["route"])
            if q["route"]
            else q["next_leg"] == 0 and (q["schema"] == 2 or q["epoch"] == 0),
            "invalid empty route generation",
        )
        require(
            not flags & ~128
            and (not flags or q["route"])
            and not any(words[3:16])
            and not any(words[17:26])
            and not any(words[27:32])
            and (
                not any(words[44:47])
                or q["schema"] == 2
                and q["origin_state"] == 5
                and tile(q["origin"])
            )
            and not any(words[47:]),
            "nonzero empty route state",
        )
        return
    require(
        q["life"] == life
        and 0 < q["route"]
        and 0 < q["next_leg"]
        and q["attempt"] > 0
        and q["epoch"] >= q["route"] + q["attempt"] - 1
        and 1 <= q["phase"] <= 7
        and cursor < count
        and not flags & ~511
        and q["family"] <= 2
        and q["tracking"] <= 1
        and q["progress"] <= 1,
        "stale actor/head/execution identity",
    )
    require(
        q["failure"] <= 8 and (q["air_mode"] or ((q["phase"] == 4) == (q["failure"] != 0))),
        "invalid canonical failure state",
    )
    require(cycle == NO_CYCLE or cycle < count, "invalid cycle boundary")
    require(
        (cycle == 0 or cursor == 0)
        and (cycle != NO_CYCLE or not flags & 3)
        and (not flags & 2 or flags & 1),
        "invalid logical patrol cursor/implicit state",
    )
    require(
        (cycle == 0 and (count == 1 or flags & 2) and tile(q["implicit"]))
        if flags & 1
        else not any(q["implicit"]),
        "invalid implicit endpoint",
    )
    if flags & EXCURSION:
        require(
            tile(q["anchor"])
            and q["ex_target"][0] != 0
            and target(q["ex_target"])
            and q["ex_objective"] <= 2
            and q["nodes"][cursor][0] == 4
            and q["phase"] in (2, 4, 5, 6),
            "invalid primary excursion state",
        )
    else:
        require(
            not any(q["anchor"] + q["ex_target"])
            and not q["ex_objective"]
            and not flags & RETURN_MODE,
            "unowned excursion metadata",
        )
    require(
        (q["phase"] != 5 or flags & EXCURSION)
        and (q["phase"] != 6 or flags & RETURN_MODE)
        and (not flags & RETURN_MODE or flags & EXCURSION and q["phase"] in (2, 4, 6))
        and (q["phase"] != 1 or not flags & APPLIED)
        and (q["phase"] not in (2, 3, 5) or flags & APPLIED),
        "invalid phase ownership",
    )
    require(
        not flags & 32
        or q["phase"] == 1
        and count >= 2
        and cursor == 0
        and not flags & APPLIED
        and not q["valid"],
        "invalid external pending ownership",
    )
    require(
        not flags & 64 or not flags & (APPLIED | 32) and q["phase"] in (1, 4),
        "invalid replacement pending ownership",
    )
    require(
        not flags & 256 or flags & APPLIED and q["phase"] in (2, 3, 6, 7),
        "planner success lacks applied execution",
    )
    require(q["valid"] <= 1 and q["placement"] <= 3, "invalid assignment enum")
    if q["valid"]:
        require(
            flags & APPLIED
            and flags & 256
            and q["placement"] > 0
            and tile(q["assigned"])
            and q["phase"] in (2, 3, 6),
            "invalid finite assignment",
        )
        if q["placement"] == 1 and q["nodes"][cursor][0] not in (2, 3):
            require(q["assigned"] == destination(q), "finite requested assignment changed goal")
    else:
        require(
            not any(q["assigned"]) and q["placement"] == q["path"] == 0,
            "unowned finite assignment metadata",
        )
    if q["tracking"]:
        require(
            cycle == 0 and q["circuit_cursor"] < count and tile(q["circuit_start"]),
            "invalid finite circuit tracker",
        )
    else:
        require(
            not any([q["circuit_cursor"], *q["circuit_start"], q["progress"]]),
            "unowned circuit tracker metadata",
        )
    require(not flags & 1 or q["tracking"], "implicit patrol lacks circuit tracking")
    require(
        cycle != 0 or count != 1 or bool(flags & 1) == bool(q["tracking"]),
        "one-point implicit/tracking mismatch",
    )
    require(
        cycle != 0
        or not (flags & APPLIED or q["phase"] == 7)
        or q["tracking"]
        and (count != 1 or flags & 1),
        "applied patrol lacks circuit tracking",
    )
    legs = set()
    for i, n in enumerate(q["nodes"]):
        require(
            1 <= n[0] <= 4
            and tile(n[1:3])
            and target(n[3:7])
            and n[3] == ({2: 1, 3: 2}.get(n[0], 0))
            and 0 < n[9] <= q["next_leg"]
            and n[9] not in legs
            and n[8] <= q["highwater"][q["ticket_owner"]]
            and n[10] <= 2
            and (n[0] in (2, 3) or n[10] == 0)
            and n[11] == 0
            and ((n[0] == 4) == (cycle != NO_CYCLE and i >= cycle)),
            "invalid canonical task node",
        )
        legs.add(n[9])
    if (q["schema"] == 1 or q["family"] != 2) and cycle != NO_CYCLE and count - cycle >= 2:
        require(
            any(n[1:3] != q["nodes"][cycle][1:3] for n in q["nodes"][cycle + 1 :]),
            "degenerate finite patrol circuit",
        )
    os, mode = q["origin_state"], q["air_mode"]
    require(os <= 6 and os != 5 and mode <= 5, "invalid origin/flight enum")
    if cycle == NO_CYCLE:
        require(not any(words[44:]), "flight metadata on a nonpatrol route")
        return
    if q["schema"] == 1:
        return
    require(q["spec_revision"] > 0, "patrol lacks specification revision")
    root = q["nodes"][cycle][8]
    if os in (1, 2, 3, 6):
        require(
            os != 1 or root <= q["frontier"][q["ticket_owner"]],
            "frozen shared origin exceeds canonical frontier",
        )
        require(
            tile(q["origin"]) and (os not in (1, 6) or root > 0) and (os != 2 or root == 0),
            "invalid resolved origin/provenance",
        )
    else:
        require(not any(q["origin"]), "unresolved origin contains a fabricated position")
    if not mode:
        require(
            not any(words[47:56])
            and q["air_slot_plus1"] == q["air_spec_count"] == 0
            and q["return_heading"] < 24
            and (not q["return_heading"] or flags & EXCURSION),
            "inactive flight contains live stream metadata",
        )
        return
    require(
        os in (1, 2, 3, 6)
        and q["family"] == 2
        and cycle == 0
        and 0 < q["air_spec_count"] <= count
        and tile(q["join_start"])
        and q["join_heading"] < 24
        and q["return_heading"] < 24
        and q["chunk_ack"] <= CHUNK
        and q["join_progress"] <= 299
        and q["join_phase"] < SLOTS * 299
        and q["curve_index"] < SLOTS * 299
        and 0 < q["chunk_revision"] <= q["spec_revision"]
        and q["air_slot_plus1"] > 0
        and flags & APPLIED
        and q["phase"] in (3, 6)
        and q["valid"] == 0,
        "invalid guided flight ownership/geometry",
    )
    require(
        (mode <= 3 and q["failure"] == 0 or mode in (4, 5) and q["failure"] == 1)
        and (bool(flags & RETURN_MODE) == (mode in (3, 5))),
        "invalid recovery/return state",
    )


def destination(q):
    if q["flags"] & RETURN_MODE:
        return q["anchor"]
    if q["flags"] & 2:
        return q["implicit"]
    return q["nodes"][q["cursor"]][1:3]


class FlightProof:
    """Observe physical starts and single native boundary crossings.

    Sampled micro31 remains direct evidence. Missing31 may be inferred only under
    an explicit caller-supplied minimum_native_microstep_cost precondition, using
    the actual native budget (current clock minus prior unit activity clock).
    This value is NOT verified from CFG here: research/upgrade speed writers can
    invalidate it. Its provenance must identify the vehicle/configuration and the
    reason the lower bound remains valid throughout the requested window.
    No planned geometry is replayed to manufacture a visit or settled snapshot.
    """

    def __init__(self, minimum_native_microstep_cost=None):
        require(
            minimum_native_microstep_cost is None
            or isinstance(minimum_native_microstep_cost, (int, float))
            and not isinstance(minimum_native_microstep_cost, bool)
            and math.isfinite(minimum_native_microstep_cost)
            and minimum_native_microstep_cost > 0,
            "minimum_native_microstep_cost must be a positive finite number",
        )
        self.minimum_native_microstep_cost = minimum_native_microstep_cost
        self.boundaries = []
        self.segment = None
        self.poses = {}
        self.counts = {
            "segments": 0,
            "joining": 0,
            "curve": 0,
            "returning": 0,
            "recovery": 0,
            "visits": 0,
            "circuits": 0,
            "segments_sampled31": 0,
            "segments_inferred_crossing": 0,
            "visits_sampled31": 0,
            "visits_inferred_crossing": 0,
            "restarts_sampled31": 0,
            "restarts_inferred_crossing": 0,
        }
        self.cycle_key, self.cycle_progress = None, 0
        self.cycle_pose = None
        self.cycle_vertex = None
        self.seen = False

    @staticmethod
    def logical(q):
        return q["cursor"], bool(q["flags"] & 2)

    @staticmethod
    def owner(q):
        return q["ticket"][:7], q["attempt"], q["epoch"]

    @staticmethod
    def physical_edge(old_u, u, shape):
        # Initial attachment may allocate a different slot from the old finite
        # path. Guided credits independently require exact physical slot identity.
        if not (
            u["state"] == 17
            and u["order"] == 16
            and 0 <= old_u["move_microstep"] <= 31
            and 0 <= u["move_microstep"] <= 31
            and old_u["move_heading"] < 24
            and u["move_heading"] < 24
            and u["native_slot"] != NO_CYCLE
        ):
            return False
        successors = DIRECTIONS[old_u["move_heading"]][2:]
        if old_u["move_heading"] >= 8:
            successors = successors[:1]
        dx, dy, *_ = DIRECTIONS[u["move_heading"]]
        return u["move_heading"] in successors and u["xy"] == [
            (old_u["x"] + dx) % shape["width"],
            (old_u["y"] + dy) % shape["height"],
        ]

    def boundary(self, old_step, step, old, old_u, q, u, credited, refill=False):
        sampled = old_u["move_microstep"] == 31
        budget = u["game_clock"] - old_u["activity_clock"]
        require(
            sampled
            or self.minimum_native_microstep_cost is not None
            and math.isfinite(budget)
            and 0 < budget < 32 * self.minimum_native_microstep_cost,
            "insufficient sampling: missing31 crossing lacks the explicit native-cost/budget bound",
        )
        kind = "sampled31" if sampled else "inferred_crossing"
        receipt = {
            "kind": kind,
            "credited": credited,
            "before_step": old_step,
            "after_step": step,
            "xy": old_u["xy"],
            "heading": old_u["move_heading"],
            "micro_before": old_u["move_microstep"],
            "micro_after": u["move_microstep"],
            "native_slot": u["native_slot"],
            "cursor_before": old_u["path_cursor"],
            "cursor_after": u["path_cursor"],
            "ack_before": old["chunk_ack"],
            "ack_after": q["chunk_ack"],
            "refill": refill,
            "native_body_budget": budget if math.isfinite(budget) else None,
        }
        self.boundaries.append(receipt)
        self.counts[("segments_" if credited else "restarts_") + kind] += 1
        return receipt

    def observe(self, previous, q, u, step, shape):
        result = {"visit": False, "returned": False, "boundary": None}
        if not previous:
            return result
        old_step, old, old_u = previous
        require(old_step + 1 == step, "guided proof requires consecutive physical frames")
        old_mode, mode = old["air_mode"], q["air_mode"]
        same_owner = self.owner(old) == self.owner(q)
        logical_changed = self.logical(old) != self.logical(q)
        physical_edge = self.physical_edge(old_u, u, shape)
        next_segment = False
        if mode:
            self.seen = True
            require(
                q["air_slot_plus1"] < 255 or u["path_slot_id"] == 254,
                "missing unambiguous physical extended path-slot evidence for guided full slot",
            )
            require(
                u["state"] == 17
                and u["order"] == 16
                and u["move_heading"] < 24
                and u["native_slot"] + 1 == q["air_slot_plus1"]
                and q["chunk_ack"] <= u["path_cursor"] <= min(CHUNK, q["chunk_ack"] + 1),
                "guided canonical/native stream identity differs",
            )
        if old_mode and mode and same_owner:
            require(
                q["chunk_revision"] >= old["chunk_revision"]
                and q["spec_revision"] >= old["spec_revision"],
                "guided revision regressed",
            )
            require(
                q["air_slot_plus1"] == old["air_slot_plus1"],
                "owned guided stream changed native slot",
            )
            ack = old["chunk_ack"]
            ordinary_credit = q["chunk_ack"] == ack + 1
            refill_credit = ack == CHUNK - 1 and q["chunk_ack"] == 0
            credited = ordinary_credit or refill_credit
            if credited:
                require(
                    old_u["path_cursor"] == ack + 1
                    and old_u["native_slot"] + 1 == old["air_slot_plus1"]
                    and physical_edge
                    and (
                        ordinary_credit
                        and u["path_cursor"] == old_u["path_cursor"] + 1
                        or refill_credit
                        and old_u["path_cursor"] == CHUNK
                        and u["path_cursor"] == 1
                    ),
                    "insufficient sampling: acknowledgement lacks one legal physical boundary crossing",
                )
                witness = (
                    self.owner(old),
                    old["chunk_revision"],
                    old["air_slot_plus1"],
                    ack + 1,
                    tuple(old_u["xy"]),
                    old_u["move_heading"],
                )
                require(
                    self.segment == witness,
                    "insufficient sampling: acknowledgement lacks an observed physical segment start",
                )
                self.segment = None
                result["boundary"] = self.boundary(
                    old_step, step, old, old_u, q, u, True, refill_credit
                )
                next_segment = True
                self.counts["segments"] += 1
                self.counts[
                    {1: "joining", 2: "curve", 3: "returning", 4: "recovery", 5: "recovery"}[
                        old_mode
                    ]
                ] += 1
                if old["chunk_revision"] == q["chunk_revision"] and q["chunk_ack"]:
                    require(
                        mode in {1: (1, 2), 2: (2,), 3: (3, 1), 4: (4,), 5: (5,)}[old_mode],
                        "insufficient sampling: connector/curve/recovery boundaries are not individually observed",
                    )
                    if old_mode == 2:
                        require(
                            mode == 2
                            and (
                                q["curve_index"] == old["curve_index"] + 1
                                or old["curve_index"] > 0
                                and q["curve_index"] == 0
                            ),
                            "insufficient sampling: curve progress is not one individually observed segment",
                        )
                    elif old_mode in (1, 3) and mode == old_mode:
                        require(
                            q["join_progress"] == old["join_progress"] + 1,
                            "insufficient sampling: connector progress is not one individually observed segment",
                        )
                    elif old_mode in (4, 5):
                        require(
                            mode == old_mode
                            and (
                                q["curve_index"] == old["curve_index"] + 1
                                or q["curve_index"] == 0
                                and old["curve_index"] > 0
                            ),
                            "insufficient sampling: recovery progress is not one individually observed segment",
                        )
                if old_mode == 2:
                    key = (
                        self.owner(old),
                        old["chunk_revision"],
                        old["air_spec_count"],
                        old["curve_index"],
                    )
                    pose = (tuple(old_u["xy"]), old_u["move_heading"])
                    require(
                        key not in self.poses or self.poses[key] == pose,
                        "same guided geometry index reached different physical pose/heading",
                    )
                    self.poses[key] = pose
                returning = bool(old["flags"] & RETURN_MODE)
                result["returned"] = returning and old_mode == 3 and not q["flags"] & RETURN_MODE
                if result["returned"]:
                    require(
                        old_u["xy"] == old["anchor"]
                        and old_u["move_heading"] == old["return_heading"]
                        and q["phase"] == 3
                        and mode in (1, 2)
                        and not q["flags"] & EXCURSION
                        and not any(q["reservation"])
                        and u["target_ref"] == 0
                        and u["committed_weapon_damage_est"] == 0,
                        "guided return did not settle at exact saved anchor/heading with cleanup",
                    )
                expected_visit = (
                    not returning and old_mode in (1, 2) and old_u["xy"] == destination(old)
                )
                require(
                    logical_changed == expected_visit,
                    "logical patrol head did not match actual nonrecovery waypoint passage",
                )
                if expected_visit:
                    require(
                        q["count"] >= old["count"]
                        and q["nodes"][: old["count"]] == old["nodes"]
                        and q["origin"] == old["origin"]
                        and (
                            q["count"] == old["count"] or q["spec_revision"] > old["spec_revision"]
                        ),
                        "guided membership changed without an append-only revision",
                    )
                    expected_cursor = (
                        old["cursor"] if q["count"] == 1 else (old["cursor"] + 1) % q["count"]
                    )
                    expected_implicit = not bool(old["flags"] & 2) if q["count"] == 1 else False
                    require(
                        self.logical(q) == (expected_cursor, expected_implicit)
                        and q["attempt"] == old["attempt"]
                        and q["epoch"] == old["epoch"],
                        "guided visit changed route ownership or skipped logical points",
                    )
                    result["visit"] = True
                    self.counts["visits"] += 1
                    self.counts["visits_" + result["boundary"]["kind"]] += 1
                    cycle_key = (
                        self.owner(old),
                        old["spec_revision"],
                        old["chunk_revision"],
                        old["air_spec_count"],
                        tuple(tuple(n) for n in old["nodes"]),
                        tuple(old["origin"]),
                    )
                    vertex = int(bool(old["flags"] & 2)) if old["count"] == 1 else old["cursor"]
                    vertices = 2 if old["count"] == 1 else old["count"]
                    if self.cycle_key != cycle_key:
                        self.cycle_key, self.cycle_progress, self.cycle_pose = cycle_key, 0, None
                    actual_pose = (tuple(old_u["xy"]), old_u["move_heading"])
                    # A completed append may first expose vertex1 or2. Anchor
                    # the actual first observed vertex, not a hypothetical0 pose.
                    # Closure still needs every ordered vertex plus the exact
                    # same physical anchor tile AND heading again.
                    expected_vertex = (
                        None
                        if self.cycle_pose is None
                        else (self.cycle_vertex + self.cycle_progress) % vertices
                    )
                    if self.cycle_pose is None or vertex != expected_vertex:
                        self.cycle_vertex = vertex
                        self.cycle_progress, self.cycle_pose = 1, actual_pose
                    elif self.cycle_progress < vertices:
                        self.cycle_progress += 1
                    else:
                        if (
                            self.cycle_pose == actual_pose
                            and q["spec_revision"] == old["spec_revision"]
                        ):
                            self.counts["circuits"] += 1
                        self.cycle_progress, self.cycle_pose = 1, actual_pose
                    if q["spec_revision"] != old["spec_revision"]:
                        self.cycle_key, self.cycle_progress, self.cycle_pose = None, 0, None
                elif old_mode in (3, 4, 5):
                    self.cycle_key, self.cycle_progress = None, 0
            else:
                require(
                    not logical_changed,
                    "insufficient sampling: logical visit lacks an individually observed acknowledgement",
                )
                require(
                    q["chunk_ack"] == ack or q["chunk_ack"] == 0,
                    "insufficient sampling: multiple native acknowledgements may lie between recorded frames",
                )
                metadata = (
                    "air_mode",
                    "join_start",
                    "join_heading",
                    "join_phase",
                    "join_progress",
                    "curve_index",
                    "chunk_revision",
                    "air_spec_count",
                )
                if any(q[k] != old[k] for k in metadata) or q["chunk_ack"] != ack:
                    require(
                        q["chunk_ack"] == 0
                        and q["air_mode"] in (1, 3, 4, 5)
                        and u["path_cursor"] == 1
                        and physical_edge
                        and q["join_start"] == old_u["xy"]
                        and q["join_heading"] == old_u["move_heading"],
                        "insufficient sampling: restart lacks its actual anchor and single physical successor",
                    )
                    witness = (
                        self.owner(old),
                        old["chunk_revision"],
                        old["air_slot_plus1"],
                        old_u["path_cursor"],
                        tuple(old_u["xy"]),
                        old_u["move_heading"],
                    )
                    require(
                        self.segment == witness,
                        "insufficient sampling: restart lost its previously observed physical segment",
                    )
                    result["boundary"] = self.boundary(old_step, step, old, old_u, q, u, False)
                    next_segment = True
                    # No acknowledgement means no visit/circuit credit. Do not
                    # bridge a circuit proof across an uncredited restart.
                    self.cycle_key, self.cycle_progress, self.cycle_pose = None, 0, None
        elif old_mode and logical_changed and q["count"] and q["route"] == old["route"]:
            require(
                False,
                "insufficient sampling: logical visit spans an unobserved suspension/restart boundary",
            )
        # A bounded native crossing proves completion plus the next actual start,
        # without manufacturing an intermediate31 snapshot. An unchanged settled
        # frame is not a new start. Activation still uses a sampled31 anchor.
        if (
            mode
            and (next_segment or old_u["move_microstep"] == 31 and u["move_microstep"] < 31)
            and u["path_cursor"] == q["chunk_ack"] + 1
        ):
            if not old_mode or not same_owner:
                require(
                    q["chunk_ack"] == 0
                    and u["path_cursor"] == 1
                    and q["join_start"] == old_u["xy"]
                    and q["join_heading"] == old_u["move_heading"],
                    "insufficient sampling: activation lacks its actual sampled settled anchor",
                )
            require(
                u["move_heading"] < 24 and old_u["move_heading"] < 24,
                "insufficient sampling: segment start lacks its individual settled physical boundary",
            )
            require(
                physical_edge,
                "insufficient sampling: sampled tile/heading cannot prove one legal native segment",
            )
            self.segment = (
                self.owner(q),
                q["chunk_revision"],
                q["air_slot_plus1"],
                u["path_cursor"],
                tuple(u["xy"]),
                u["move_heading"],
            )
        elif mode and self.segment and same_owner:
            require(
                (u["native_slot"] + 1, u["path_cursor"], tuple(u["xy"]), u["move_heading"])
                == (self.segment[2], self.segment[3], self.segment[4], self.segment[5]),
                "insufficient sampling: pose/cursor/slot changed before an individually witnessed acknowledgement",
            )
        elif not mode or not same_owner:
            self.segment = None
        return result


def decode_records(evidence, actor, first, last):
    """Preserve both phases and exact68-byte records; expose only actual issuer receipts."""
    out = []
    for sample in evidence.samples:
        if not first <= sample.step <= last:
            continue
        for payload in sample.records:
            slot, owner, param, code = struct.unpack_from("<HHhH", payload, 8)
            if slot != actor[1] or owner & 15 != actor[0]:
                continue
            args = list(struct.unpack_from("<13I", payload, 16))
            if owner == actor[0] and param == 254 and code == 254:
                tag = args[0]
                require(
                    tag & 255 in (1, 2)
                    and tag >> 24 == 0
                    and args[11:] == [0, 0]
                    and not args[5] & 0xFF00FF00
                    and not args[6] & 0xFFFFF800
                    and 1 <= (tag >> 8 & 255) <= 3
                    and args[4] > 0
                    and args[3] < 32
                    and args[10] > 0,
                    "malformed actual task-edit ingress",
                )
                require(args[1] == actor[2], "task edit addressed another actor lifetime")
                sig = [
                    tag >> 16 & 255,
                    args[5] & 255,
                    args[5] >> 16,
                    args[6] & 255,
                    args[6] >> 8,
                    args[7],
                    args[8],
                ]
                operation, kind = tag >> 8 & 255, sig[0]
                require(
                    (operation == 3 and not any(sig) and args[9] == 0)
                    or (
                        operation != 3
                        and 1 <= kind <= 4
                        and sig[3] == {2: 1, 3: 2}.get(kind, 0)
                        and (not any(sig[3:]) if sig[3] == 0 else sig[5] < 65535 and sig[6] > 0)
                    ),
                    "malformed actual semantic edit grammar",
                )
                out.append(
                    {
                        "step": sample.step,
                        "phase": sample.phase,
                        "raw": payload.hex(),
                        "words": args,
                        "schema": tag & 255,
                        "legacy_sequence": (tag & 255) == 1,
                        "edit": tag >> 8 & 255,
                        "signature": sig,
                        "world": args[2:5],
                        "group": args[9],
                        "sequence": args[10],
                    }
                )
            # Same finite constructor whitelist as hosted family_of/decode_applied:
            # walkers marshal through0A/15; planes and helicopters use0B/17 or0B/18.
            elif owner == (128 | actor[0]) and (code, param) in ((10, 15), (11, 17), (11, 18)):
                out.append(
                    {
                        "step": sample.step,
                        "phase": sample.phase,
                        "raw": payload.hex(),
                        "ordinary": True,
                        "code": code,
                        "param": param,
                        "xy": args[:2],
                        "group": args[12],
                    }
                )
    return out


def check(target, spec, selected=None, peer=None, peer_session=None):
    actor = spec["actor"]
    require(
        len(actor) == 3 and all(isinstance(x, int) for x in actor),
        "actor must be explicit player,slot,lifetime",
    )
    first, last = spec["steps"]
    require(1 <= first < last, "required step bounds are invalid")
    expected = spec["nodes"]
    require(
        expected and all(len(n) == 7 for n in expected),
        "nodes must be explicit seven-word task identities",
    )
    executor = spec["executor"]
    require(
        executor in (0, 1, 2), "required executor must be explicit ground/nonplane/plane family"
    )
    e = Evidence(target, selected)
    require(last <= e.rec.last_step, "required workload step coverage is missing")
    ingress = decode_records(e, actor, first, last)
    selected_route, admitted, previous, assignment, return_assignment = None, None, None, None, None
    live_attack, destroyed, cleanup = {}, {}, {}
    excursion, returned, resumed = None, None, None
    ready, completed, cursor_changes = {}, set(), []
    replacement, last_clock, observed = None, None, 0
    events = []
    window_world = None
    minimum_native_microstep_cost = spec.get("minimum_native_microstep_cost")
    flight = FlightProof(minimum_native_microstep_cost)
    physical_previous = None
    for step, buf in frames(e.rec):
        observed += 1
        require(step == observed, "state record does not cover every full session step")
        clock_bytes = bytes(e.region(buf, "game_clock"))
        require(
            clock_bytes == e.clocks[(step - 1) * 8 : step * 8],
            f"state/recorded clock disagreement at step{step}",
        )
        clock = struct.unpack("<d", clock_bytes)[0]
        require(
            math.isfinite(clock) and (last_clock is None or clock > last_clock),
            "nonfinite/nonincreasing recorded game clock",
        )
        last_clock = clock
        if not first <= step <= last:
            continue
        q = e.row(buf, actor)
        if window_world is None:
            window_world = e.shape["world"]
        require(
            window_world == e.shape["world"],
            "world changed inside required workload bounds; provide separate explicit save/load windows",
        )
        u = e.unit(buf, actor[0], actor[1])
        if q["count"] and selected_route is None and [signature(n) for n in q["nodes"]] == expected:
            selected_route, admitted = q["route"], step
            for index, n in enumerate(q["nodes"]):
                direct = any(
                    r.get("signature") == signature(n)
                    and (r.get("edit") == 2 or index == 0 and r.get("edit") == 1)
                    and r["world"] == e.shape["world"]
                    and r["group"] == n[7]
                    and r["sequence"] == n[8]
                    and r["step"] <= step
                    for r in ingress
                )
                retained = (
                    index == 0
                    and n[0] == 1
                    and any(
                        r.get("ordinary")
                        and r["xy"] == n[1:3]
                        and r["group"] == n[7]
                        and r["step"] <= step
                        for r in ingress
                    )
                )
                require(
                    direct or retained,
                    "canonical admitted node lacks matching actual native/MHOR ingress",
                )
        if selected_route is None:
            physical_previous = (step, q, u)
            continue
        require(
            u["unit_proto_id"] > 0 and u["hp"] > 0,
            f"actor died during the observed chain at step{step}",
        )
        actor_life, actor_flags = e.identity(buf, 1, actor[0], actor[1])
        require(
            actor_life == actor[2] and actor_flags & 1,
            "required live actor incarnation no longer exists",
        )
        if replacement is not None:
            require(q["count"] == 0, "old route reappeared after explicit replacement")
            previous = (step, q, u)
            continue
        require(q["route"] == selected_route, "unexpected route generation replaced required chain")
        n = q["active"]
        if n:
            require(
                q["family"] == executor,
                "actual canonical executor differs from required actor family",
            )
        guided = flight.observe(previous or physical_previous, q, u, step, e.shape)
        if q["valid"]:
            require(
                q["flags"] & APPLIED and q["placement"] in (1, 2, 3) and q["family"] in (0, 1, 2),
                "assignment lacks applied/matched executor identity",
            )
            require(
                q["path"] == u["native_slot"]
                or q["schema"] == 1
                and q["path"] == u["path_slot_id"] == 255,
                "assignment/native path-slot identity differs",
            )
            witness = {
                "step": step,
                "ticket": q["ticket"],
                "assigned": q["assigned"],
                "placement": q["placement"],
                "path": q["path"],
                "family": q["family"],
            }
            if q["phase"] == 6:
                return_assignment = witness
            else:
                assignment = witness
        if n and n[0] in (1, 4) and not q["air_mode"]:
            matched = assignment and assignment["ticket"] == q["ticket"]
            physical = matched and u["xy"] == assignment["assigned"] and u["move_microstep"] == 31
            # Actual one-pass empty-path travel can publish and retire between snapshots.
            zero = (
                q["phase"] == 7
                and q["flags"] & APPLIED
                and u["xy"] == n[1:3]
                and [u["goal_x"], u["goal_y"]] == n[1:3]
                and u["move_microstep"] == 31
                and u["path_slot_id"] == 255
            )
            if physical or zero:
                ready[(q["route"], n[9])] = {
                    "step": step,
                    "ticket": q["ticket"],
                    "xy": u["xy"],
                    "zero_distance": bool(zero),
                    "assignment": assignment if physical else None,
                }
        if n and n[0] in (2, 3):
            target = n[3:7]
            key = (q["route"], n[9])
            hp = e.target_hp(buf, target)
            ref = target[1] | (128 if target[0] == 1 else 64)
            if (
                q["flags"] & APPLIED
                and q["phase"] == 3
                and n[10] == 0
                and hp > 0
                and u["target_ref"] == ref
                and u["target_index"] == target[2]
            ):
                require(
                    q["reservation"][:5] == [2] + target,
                    "actual attack lacks exact-lifetime bound reservation",
                )
                live_attack.setdefault(key, {"step": step, "target": target, "ticket": q["ticket"]})
            if key in live_attack and n[10] == 1:
                require(hp <= 0, "destruction objective lacks recorded real HP death")
                destroyed.setdefault(key, step)
            if key in destroyed and q["phase"] == 7:
                require(
                    not q["flags"] & APPLIED
                    and not any(q["reservation"])
                    and u["target_ref"] == 0
                    and u["committed_weapon_damage_est"] == 0,
                    "attack advanced without physical cleanup/accounting withdrawal",
                )
                cleanup.setdefault(key, step)
        if q["phase"] == 5 and excursion is None:
            require(
                n and n[0] == 4 and q["flags"] & EXCURSION and q["flags"] & APPLIED,
                "excursion is not owned patrol primary combat",
            )
            t = q["ex_target"]
            require(
                t[0] in (1, 2)
                and e.target_hp(buf, t) > 0
                and u["target_index"] == t[2]
                and u["target_ref"] == (t[1] | (128 if t[0] == 1 else 64)),
                "excursion has no real live native primary target",
            )
            require(
                q["reservation"][:5] == [2] + t,
                "excursion lacks exact-lifetime actual primary reservation",
            )
            excursion = {
                "step": step,
                "route": q["route"],
                "leg": n[9],
                "anchor": q["anchor"],
                "target": t,
            }
        if excursion and returned is None and previous:
            if guided["returned"]:
                require(
                    n and n[9] == excursion["leg"] and q["route"] == excursion["route"],
                    "guided return resumed another route/leg",
                )
                returned = step
            old_step, old, _old_u = previous
            if old["phase"] == 6 and q["phase"] == 1 and not old["air_mode"]:
                require(
                    return_assignment and old["ticket"] == return_assignment["ticket"],
                    "return settled without a matched fresh assignment",
                )
                require(
                    n
                    and n[9] == excursion["leg"]
                    and q["route"] == excursion["route"]
                    and u["xy"] == return_assignment["assigned"]
                    and u["move_microstep"] == 31
                    and [u["goal_x"], u["goal_y"]] == excursion["anchor"],
                    "return did not actually reach its assigned saved-anchor boundary",
                )
                require(
                    not any(q["reservation"]) and u["target_ref"] == 0,
                    "excursion primary accounting leaked into return completion",
                )
                returned = step
        if (
            returned
            and step > returned
            and resumed is None
            and n
            and n[9] == excursion["leg"]
            and q["phase"] in (3, 7)
            and q["flags"] & APPLIED
        ):
            require(
                q["route"] == excursion["route"] and step > returned,
                "wrong patrol leg resumed after excursion",
            )
            resumed = step
        if previous:
            old_step, old, _old_u = previous
            old_n = old["active"]
            changed = old_n and (
                n is None or n[9] != old_n[9] or bool(old["flags"] & 2) != bool(q["flags"] & 2)
            )
            if changed:
                key = (old["route"], old_n[9])
                ordinary = [
                    r
                    for r in ingress
                    if r.get("ordinary")
                    and r["xy"] == spec.get("replacement")
                    and old_step <= r["step"] <= step
                ]
                if not q["count"] and ordinary and spec.get("replacement") is not None:
                    require(
                        not any(q["reservation"]) and not q["flags"] and u["hp"] > 0,
                        "explicit replacement failed to clear live ownership",
                    )
                    replacement = step
                elif old_n[0] in (2, 3):
                    require(
                        key in live_attack and key in destroyed and key in cleanup,
                        "attack consumed without live execution/destruction/cleanup",
                    )
                    completed.add(key)
                elif old["air_mode"]:
                    require(
                        guided["visit"],
                        "guided head advanced without proven physical waypoint passage",
                    )
                    completed.add(key)
                    cursor_changes.append(
                        {
                            "step": step,
                            "before": old["cursor"],
                            "after": q["cursor"],
                            "old_leg": old_n[9],
                            "new_leg": n[9],
                            "attempt": q["attempt"],
                            "guided": True,
                            "boundary": guided["boundary"],
                            "implicit_before": bool(old["flags"] & 2),
                            "implicit_after": bool(q["flags"] & 2),
                        }
                    )
                else:
                    # Last finite travel may retire in this very sample; native final idle may
                    # then move in the same step, so retain the prior exact settled witness too.
                    terminal = (
                        assignment
                        and assignment["ticket"] == old["ticket"]
                        and u["xy"] == assignment["assigned"]
                        and u["move_microstep"] == 31
                    )
                    settled = key in ready and ready[key]["ticket"] == old["ticket"]
                    require(
                        settled or terminal,
                        f"head/cursor advanced without matched assignment + actual settled pose at step{step}: old={old['ticket']} phase={old['phase']} ready={ready.get(key)} assignment={assignment}",
                    )
                    completed.add(key)
                    if old_n[0] == 4 and n and n[0] == 4:
                        require(
                            old["count"] == q["count"] == 2
                            and old["cycle"] == q["cycle"] == 0
                            and q["attempt"] > old["attempt"],
                            "patrol cursor changed without a fresh two-point cycle leg",
                        )
                        cursor_changes.append(
                            {
                                "step": step,
                                "before": old["cursor"],
                                "after": q["cursor"],
                                "old_leg": old_n[9],
                                "new_leg": n[9],
                                "attempt": q["attempt"],
                            }
                        )
        if not previous or tuple(q[k] for k in ("phase", "attempt", "count", "cursor")) != tuple(
            previous[1][k] for k in ("phase", "attempt", "count", "cursor")
        ):
            events.append(
                {
                    "step": step,
                    "phase": q["phase"],
                    "attempt": q["attempt"],
                    "count": q["count"],
                    "cursor": q["cursor"],
                    "xy": u["xy"],
                    "hp": u["hp"],
                }
            )
        previous = (step, q, u)
    require(observed == e.rec.last_step, "state-record full coverage mismatch")
    require(admitted is not None, "required exact task list was never admitted")
    attack_count = sum(n[0] in (2, 3) for n in expected)
    require(
        len(cleanup) >= attack_count,
        "required attacks did not execute alive and complete confirmed cleanup",
    )
    if spec.get("excursion"):
        require(
            excursion and returned and resumed,
            "missing actual primary excursion, settled return, or same-leg resumption",
        )
    finite_circuits = sum(
        x["before"] == 1 and x["after"] == 0 and not x.get("guided") for x in cursor_changes
    )
    circuits = finite_circuits + flight.counts["circuits"]
    require(
        circuits >= spec.get("cycles", 0), "missing complete physically witnessed patrol circuits"
    )
    if flight.seen and spec.get("replacement") is None:
        require(
            flight.counts["segments"] > 0 and flight.counts["visits"] > 0,
            "guided workload has no fully observed physical waypoint visit",
        )
    if spec.get("replacement") is not None:
        require(replacement is not None, "missing actual ordinary replacement of a live route")
    elif all(n[0] == 1 for n in expected):
        require(
            previous and previous[1]["count"] == 0 and len(completed) >= len(expected),
            "finite move chain did not physically finish every leg",
        )
    pair = None
    if peer is not None:
        other = Evidence(peer, peer_session)
        require(
            e.rec.first_step == other.rec.first_step and e.rec.last_step == other.rec.last_step,
            "peer full-record coverage differs",
        )
        require(
            e.clocks == other.clocks and e.samples == other.samples,
            "peer clocks/full MHOR HASH+ENTRY payloads differ",
        )
        pair = sr.first_diff(
            e.rec, other.rec
        )  # Existing committed mask policy, never a new exclusion.
        require(pair.get("identical"), "peer state mismatch: " + repr(pair))
        for (step, a), (other_step, b) in zip(frames(e.rec), frames(other.rec)):
            require(step == other_step, "peer state axes differ")
            if not first <= step <= last:
                continue
            qa, qb = e.row(a, actor), other.row(b, actor)
            require(qa == qb, f"peer raw canonical actor/ticket differs at step{step}")
            for evidence, source in ((e, a), (other, b)):
                stride = evidence.layouts[sr.REGION_LAYOUTS["units"]["struct"]].size
                pitch = evidence.lengths["units"] // (evidence.shape["players"] * stride)
                at = evidence.bases["units"] + (actor[0] * pitch + actor[1]) * stride
                current = memoryview(source)[at : at + stride]
                if evidence is e:
                    actor_bytes = current
                else:
                    require(
                        actor_bytes == current, f"peer full raw actor record differs at step{step}"
                    )
    return {
        "process": str(e.process),
        "session": str(e.session),
        "actor": actor,
        "steps": [first, last],
        "recorded_steps": e.rec.last_step,
        "mhor_samples": len(e.samples),
        "admitted": admitted,
        "live_attacks": live_attack,
        "destruction_steps": destroyed,
        "cleanup_steps": cleanup,
        "excursion": excursion,
        "returned": returned,
        "resumed": resumed,
        "circuits": circuits,
        "guided_flight": flight.counts,
        "guided_flight_boundaries": flight.boundaries,
        "guided_flight_precondition": {
            "minimum_native_microstep_cost": minimum_native_microstep_cost,
            "provenance": spec.get("minimum_native_microstep_cost_provenance"),
            "verification": (
                "caller-supplied vehicle/configuration lower-bound precondition; "
                "CFG and research-speed changes are not automatically verified"
                if minimum_native_microstep_cost is not None
                else "no native-cost precondition supplied; sampled31 boundaries only"
            ),
        },
        "cursor_changes": cursor_changes,
        "replacement": replacement,
        "events": events,
        "pair": pair,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "target", help="EXPLICIT process or session directory; never a lane/log root"
    )
    parser.add_argument("--session", help="exact session leaf/path belonging to target process")
    parser.add_argument(
        "--spec",
        required=True,
        help="explicit actor, required step bounds and expected node identities JSON",
    )
    parser.add_argument(
        "peer", nargs="?", help="optional EXPLICIT peer process/session for full recorded parity"
    )
    parser.add_argument("--peer-session")
    args = parser.parse_args()
    try:
        report = check(
            args.target,
            json.loads(Path(args.spec).read_text()),
            args.session,
            args.peer,
            args.peer_session,
        )
        # Tuple keys are internal matching identities, not JSON object keys.
        for name in ("live_attacks", "destruction_steps", "cleanup_steps"):
            report[name] = [{"route_leg": list(k), "evidence": v} for k, v in report[name].items()]
        print(json.dumps(report, indent=2))
        print("order chains: PASS")
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, "order chains: FAIL: " + str(error) + "\n")


if __name__ == "__main__":
    main()
