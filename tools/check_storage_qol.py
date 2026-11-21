#!/usr/bin/env python3
"""Check real storage consumers and ordered settings in an explicit invocation.

Repair/production use native dock/world links. The settings mode additionally
requires nondefault policy on both sides of an actual live resync.
No newest-run lookup, fixture construction or HP injection.
"""

import argparse
import gzip
import hashlib
import re
import struct
from pathlib import Path

import _rundir
import check_bigmap as bigmap
import mp_analyze as mp
import mp_order_diff as order_stream
import state_record as sr


def require(condition, message):
    if not condition:
        raise ValueError(message)


def recording(target, ordinal):
    directories = _rundir.process_and_sessions(target)
    process = Path(directories[0])
    drive = (process / "mh_uidrive.log").read_text(encoding="utf-8", errors="replace")
    require("[script] COMPLETE" in drive, "this invocation did not complete its UI script")
    sessions = directories[1:]
    require(1 <= ordinal <= len(sessions), "requested session is missing from this invocation")
    session = Path(sessions[ordinal - 1])
    path = session / "mh_match_state.bin"
    if not path.is_file():
        path = session / "mh_match_state.bin.gz"
    rec = sr.load(str(path))
    require(rec.stop_reason is None, "damaged/truncated state record: " + str(rec.stop_reason))
    require(rec.first_step is not None and rec.last_step > rec.first_step, "empty state record")
    print(
        f"storage evidence: process={process} session={session} steps={rec.first_step}..{rec.last_step}"
    )
    return rec, drive


class View:
    def __init__(self, rec):
        self.rec = rec
        self.buf = None
        self.layouts = sr._get_structs()
        for region in ("units", "buildings", "unit_storage", "tile_objects"):
            require(region in rec.region_index, "missing recorded region " + region)
            layout = self.layouts[sr.REGION_LAYOUTS[region]["struct"]]
            require(layout.reliable, "unreliable generated layout for " + region)

    def field(self, region, slot, name, fmt=None):
        layout = self.layouts[sr.REGION_LAYOUTS[region]["struct"]]
        fields = [f for f in layout.fields if f.name == name]
        require(len(fields) == 1, "missing generated field " + region + "." + name)
        field = fields[0]
        length = self.rec.regions[self.rec.region_index[region]].len
        require(0 <= slot and (slot + 1) * layout.size <= length, "record index out of bounds")
        offset = (
            self.rec.region_offset[self.rec.region_index[region]]
            + slot * layout.size
            + field.offset
        )
        raw = self.buf[offset : offset + field.size]
        if fmt:
            require(struct.calcsize(fmt) == field.size, "generated field width changed: " + name)
            return struct.unpack(fmt, raw)[0]
        return int.from_bytes(raw, "little")

    def frames(self):
        for chunk in self.rec.chunks:
            if isinstance(chunk, sr.KeyframeChunk):
                self.buf = bytearray(chunk.state)
            elif isinstance(chunk, sr.StepChunk):
                require(self.buf is not None, "delta before initial keyframe")
                for rid, offset, length, data in chunk.runs:
                    base = self.rec.region_offset[rid] + offset
                    self.buf[base : base + length] = data
            else:
                continue
            yield chunk.step

    def unit(self, uid):
        # Both folded workloads are local player zero. Do not pretend a native vs
        # promoted row pitch or policy reservation is a live raised-cap proof.
        return {
            name: self.field("units", uid, name, "<d" if name == "energy" else None)
            for name in (
                "unit_proto_id",
                "state",
                "energy",
                "home_storage_slot",
                "x",
                "y",
                "goal_x",
                "goal_y",
                "elevation",
                "order",
            )
        }

    def docked(self, slot):
        count = self.field("unit_storage", slot, "docked_count", "<i")
        layout = self.layouts[sr.REGION_LAYOUTS["unit_storage"]["struct"]]
        field = next(f for f in layout.fields if f.name == "docked_units")
        require(0 <= count <= field.count and field.elem_size == 4, "invalid real docked list")
        start = (
            self.rec.region_offset[self.rec.region_index["unit_storage"]]
            + slot * layout.size
            + field.offset
        )
        return struct.unpack_from(f"<{count}i", self.buf, start) if count else ()

    def linked(self, uid, u, air):
        tile = (u["x"] << 8) | u["y"]
        if air:
            # Native tile.unit is a packed owner<<12|id chain, not a single occupant.
            layout = self.layouts[sr.REGION_LAYOUTS["units"]["struct"]]
            length = self.rec.regions[self.rec.region_index["units"]].len
            require(length % (8 * layout.size) == 0, "invalid recorded unit row pitch")
            row = length // (8 * layout.size)
            link = self.field("tile_objects", tile, "unit")
            seen = set()
            while link:
                player, slot = link >> 12, link & 4095
                require(
                    player < 8 and 0 < slot < row and link not in seen,
                    "invalid or cyclic air stack",
                )
                seen.add(link)
                index = player * row + slot
                require(
                    (self.field("units", index, "x"), self.field("units", index, "y"))
                    == (u["x"], u["y"]),
                    "air stack crosses tile coordinates",
                )
                if player == 0 and slot == uid:
                    return True
                link = self.field("units", index, "unit_above")
            return False
        owner = self.field("tile_objects", tile, "class_owner")
        return (
            owner & 0x80 and owner & 15 == 0 and self.field("tile_objects", tile, "building") == uid
        )


def travelled_to(u, origin, goal, width, height):
    def distance(pos):
        dx = abs(pos[0] - goal[0]) % width
        dy = abs(pos[1] - goal[1]) % height
        return min(dx, width - dx) ** 2 + min(dy, height - dy) ** 2

    now = (u["x"], u["y"])
    return (
        origin is not None
        and now != origin
        and distance(now) < distance(origin)
        and distance(now) <= 2
    )


def check_repair(target, ordinal=1):
    rec, drive = recording(target, ordinal)
    saves = re.findall(r"\[script\] savegame qolstorage -> (-?\d+)\b", drive)
    require(saves == ["1"], "expected one successful real qolstorage save")
    process = Path(_rundir.process_and_sessions(target)[0])
    net = (process / "mh_net.log").read_text(encoding="utf-8", errors="replace")
    loads = re.findall(r"\[desync\] BOUNDARY save_load \(qolstorage\)", net)
    require(len(loads) == 1, "expected one named qolstorage reload in this invocation")
    for label in (
        "qol_healthy_before_repair",
        "qol_damaged_before_repair",
        "qol_repaired_after",
        "qol_healthy_after",
        "qol_damaged_after",
    ):
        require(re.search(r"\[goal\] " + label + r"\b", drive), "missing real observation " + label)
    session = Path(_rundir.process_and_sessions(target)[ordinal])
    orders_path = session / "mh_rec_orders.bin"
    if not orders_path.is_file():
        orders_path = session / "mh_rec_orders.bin.gz"
    commands = order_stream.load(orders_path)
    bulk = {}
    off = []
    for q in commands:
        if q.unit_index != 30 or q.owner_and_kind & 15:
            continue
        if q.kind == 0x40 and q.order_code == q.param0 == 0xE6:
            bulk.setdefault(q.raw, q.step)
        elif q.kind == 0 and q.order_code == q.param0 == 0xFD:
            if struct.unpack_from("<i", q.raw, 24)[0] == 0:
                off.append(q.step)
    require(len(bulk) == 1, "expected one actual building30 Exit All command")
    bulk_step = next(iter(bulk.values()))
    require(off and max(off) < bulk_step, "manual bulk proof must execute with Auto off")
    view = View(rec)
    initial = None
    repaired_at = None
    released_at = None
    world_at = None
    manual_started = None
    manual_world = None
    policy_flags19 = None
    auto_arrived = None
    manual_arrived = None
    auto_origin = None
    manual_origin = None
    bulk_world = {}
    bulk_arrived = {}
    bulk_origin = {}
    rally_replaced = False
    bulk_committed = {}
    for step in view.frames():
        units = {uid: view.unit(uid) for uid in (19, 21, 22, 23)}
        docked = view.docked(3)
        require(view.field("unit_storage", 3, "b_index", "<i") == 30, "receiver changed identity")
        require(
            view.field("buildings", 30, "building_id") == 39,
            "receiver is not the real saved helipad",
        )
        if initial is None:
            initial = units
            require(
                all(u["unit_proto_id"] == 35 and u["state"] == 0x1F for u in units.values()),
                "saved repair controls were not real docked aircraft",
            )
            require(
                all(uid in docked for uid in units), "saved controls missing from real docked list"
            )
            require(
                units[19]["energy"] == 300 and 0 < units[21]["energy"] < units[23]["energy"] == 295,
                "saved HP controls do not match ayy30",
            )
            base = rec.region_offset[rec.region_index["storage_policy"]]
            header = struct.unpack_from("<8I", view.buf, base)
            policy_flags19 = base + 32 + header[2] * header[3] * 16 + 19 * 16 + 4
        u = units[21]
        if step < bulk_step:
            require(u["state"] == 0x1F and 21 in docked, "unfinished repair control was ejected")
            require(
                (u["goal_x"], u["goal_y"]) == (initial[21]["goal_x"], initial[21]["goal_y"]),
                "unfinished control received the rally before explicit bulk exit",
            )
        policy_base = rec.region_offset[rec.region_index["storage_policy"]]
        building_policy = policy_base + 32 + 30 * 16
        _, flags, rally_x, rally_y = struct.unpack_from("<4I", view.buf, building_policy)
        if step >= bulk_step:
            require(not flags & 4, "bulk proof accidentally depends on Auto being enabled")
            rally_replaced |= (rally_x, rally_y) == (73, 19) and bool(flags & 2)
            for uid in (21, 22):
                candidate = units[uid]
                if uid not in bulk_committed and candidate["state"] != 0x1F:
                    require(
                        candidate["state"] in (0x20, 0x21, 0x22, 0x27, 0x28)
                        and candidate["order"] == 0x0B
                        and (candidate["goal_x"], candidate["goal_y"]) == (69, 16),
                        "bulk launch did not commit the rally before physical departure",
                    )
                    bulk_committed[uid] = step
                if uid not in bulk_world and view.linked(uid, candidate, air=True):
                    require(
                        uid not in docked
                        and (candidate["goal_x"], candidate["goal_y"]) == (69, 16),
                        "bulk departure did not retain the accepted rally snapshot",
                    )
                    if uid == 21:
                        require(
                            candidate["energy"] < 300, "bulk damaged-unit control became healthy"
                        )
                    # Native air-chain admission starts at elevation0. Observe the first
                    # positive-height frame separately, as for the existing row-exit proof.
                    if candidate["elevation"] > 0:
                        bulk_world[uid] = step
                        bulk_origin[uid] = (candidate["x"], candidate["y"])
                if (
                    uid in bulk_world
                    and uid not in bulk_arrived
                    and view.linked(uid, candidate, air=True)
                    and (candidate["goal_x"], candidate["goal_y"]) == (69, 16)
                    and travelled_to(candidate, bulk_origin[uid], (69, 16), 128, 128)
                ):
                    bulk_arrived[uid] = step
        require(
            not struct.unpack_from("<I", view.buf, policy_flags19)[0] & 2,
            "already-healthy unit19 was Auto-qualified",
        )
        healthy = units[19]
        if manual_started is None and (healthy["state"] != 0x1F or 19 not in docked):
            require(world_at is not None, "healthy unit left before the later manual exit")
            manual_started = step
        if manual_started is None:
            require(
                (healthy["goal_x"], healthy["goal_y"])
                == (initial[19]["goal_x"], initial[19]["goal_y"]),
                "healthy control received rally before manual exit",
            )
        elif (
            manual_world is None
            and view.linked(19, healthy, air=True)
            and (healthy["goal_x"], healthy["goal_y"]) == (69, 16)
            and healthy["elevation"] > 0
        ):
            manual_world = step
            manual_origin = (healthy["x"], healthy["y"])
        u = units[23]
        require(
            u["home_storage_slot"] == 3 and u["unit_proto_id"] == 35,
            "repair candidate changed receiver/type",
        )
        if u["energy"] == 300 and repaired_at is None:
            repaired_at = step
        if 23 not in docked and released_at is None:
            require(
                repaired_at is not None and step >= repaired_at,
                "unit left before reaching full HP through repair",
            )
            released_at = step
        if (
            world_at is None
            and released_at is not None
            and view.linked(23, u, air=True)
            and (u["goal_x"], u["goal_y"]) == (69, 16)
            and u["elevation"] > 0
        ):
            world_at = step
            auto_origin = (u["x"], u["y"])
        if (
            world_at is not None
            and auto_arrived is None
            and u["energy"] > 0
            and travelled_to(u, auto_origin, (69, 16), 128, 128)
            and view.linked(23, u, air=True)
        ):
            auto_arrived = step
        if (
            manual_world is not None
            and manual_arrived is None
            and healthy["energy"] > 0
            and travelled_to(healthy, manual_origin, (69, 16), 128, 128)
            and view.linked(19, healthy, air=True)
        ):
            manual_arrived = step
        last = units
    require(
        repaired_at is not None and released_at is not None and world_at is not None,
        "full-HP repair did not release into actual world linkage with rally",
    )
    require(last[19]["energy"] == 300, "healthy control lost HP")
    require(
        initial[21]["energy"] < last[21]["energy"] < 300,
        "damaged control did not receive a real partial repair",
    )
    require(
        manual_started is not None and manual_world is not None,
        "ordinary healthy-unit row click did not exit into world linkage with rally",
    )
    require(
        auto_arrived is not None and manual_arrived is not None,
        "departure only stored a goal: repaired/manual aircraft never reached the rally",
    )
    print(
        f"storage repair: PASS full={repaired_at} released={released_at} world+rally={world_at}"
        f" auto-arrived={auto_arrived} manual-start={manual_started}"
        f" manual-world+rally={manual_world} manual-arrived={manual_arrived}"
    )
    require(
        set(bulk_committed) == set(bulk_arrived) == {21, 22} and rally_replaced,
        "bulk occupants did not reach the old rally after its replacement",
    )
    print(
        f"storage bulk: PASS Auto-off command={bulk_step} world={bulk_world}"
        f" committed={bulk_committed} arrived={bulk_arrived}; later rally replacement did not retarget departures"
    )
    # Reuse the existing Auto-On capture: another early capture shifts this
    # pinned gameplay scene by one present. Off is also judged against its
    # strict status-field baseline by ui_test, so equality proves both states.
    process = _rundir.process_and_sessions(target)[0]
    on = bigmap.load(str(process), "qol_storage__auto_on")
    off = bigmap.load(str(process), "qol_auto_hint__off")
    require(on is not None and off is not None, "missing actual Auto hover frames")
    require(on.size == off.size == (1024, 768), "Auto hint needs native1024x768 frames")
    field = (96, 736, 474, 752)
    require(
        on.crop(field).tobytes() == off.crop(field).tobytes(),
        "Auto status hint differs between its enabled and disabled states",
    )
    print("storage hover: PASS same native status hint in Auto On/Off")


def check_production(target, states, ordinal=1):
    rec, drive = recording(target, ordinal)
    marker = re.findall(r"\[sel\] qol_production_receiver cam=(\d+),(\d+) .*? bldg=(\d+)\b", drive)
    require(len(marker) == 1, "expected one real selected production receiver")
    camx, camy, bslot = map(int, marker[0])
    # cfg-a is the established 128-square map at native 100% zoom. This is
    # the actual script's ordinary world right-click, not a policy byte oracle.
    goal = (((camx + 500) // 32) % 128, ((camy + 400) // 32) % 128)
    made = states["production_finished"]
    require(made["side"] == 0 and made["unit"] > 0, "expected actual local produced Soldier")
    require("production_auto_exited" in states, "missing pre-manual-move Auto observation")
    exited = states["production_auto_exited"]
    require(
        exited["unit"] == made["unit"] and exited["goal"] == goal,
        "actual production did not acquire receiver rally before manual move",
    )
    view = View(rec)
    received_at = None
    exit_at = None
    world_at = None
    arrived_at = None
    origin = None
    for step in view.frames():
        u = view.unit(made["unit"])
        if u["unit_proto_id"] != 26 or u["energy"] <= 0:
            continue
        storage = u["home_storage_slot"]
        receiver = view.field("unit_storage", storage, "b_index", "<i")
        if receiver != bslot:
            continue
        require(
            view.field("buildings", bslot, "building_id") == 36,
            "production receiver is not the normally built Barracks",
        )
        if made["unit"] in view.docked(storage) and received_at is None:
            received_at = step
        if (
            received_at is not None
            and made["unit"] not in view.docked(storage)
            and (u["goal_x"], u["goal_y"]) == goal
        ):
            if exit_at is None:
                exit_at = step
            if view.linked(made["unit"], u, air=False):
                if world_at is None:
                    world_at = step
                    origin = (u["x"], u["y"])
                if travelled_to(u, origin, goal, 128, 128):
                    arrived_at = step
                    break
    require(
        received_at is not None
        and exit_at is not None
        and world_at is not None
        and arrived_at is not None,
        "normal production did not receive, Auto-exit and actually reach the rally",
    )
    print(
        f"storage production: PASS received={received_at} exit={exit_at} world+rally={world_at}"
        f" arrived={arrived_at} goal={goal!r}"
    )


def target_color_pixels(frame, background, color):
    """Attribute alpha-blended artwork without assuming the terrain is grey."""
    import numpy as np

    base = background.astype(np.float32)
    delta = frame.astype(np.float32) - base
    palette = np.asarray(color, dtype=np.float32)
    result = np.zeros(base.shape[:2], dtype=bool)
    # The approved ground layer lets native shadows halve an already drawn stroke.
    for shade in (1.0, 0.5, 0.25):
        direction = palette * shade - base
        norm = np.sum(direction * direction, axis=2)
        alpha = np.sum(delta * direction, axis=2) / np.maximum(norm, 1.0)
        residual = np.max(np.abs(delta - alpha[:, :, None] * direction), axis=2)
        result |= (norm >= 256) & (alpha >= 0.04) & (alpha <= 1.05) & (residual <= 9)
    return result


def check_targets(target, cropped=False):
    import numpy as np

    process = Path(_rundir.process_and_sessions(target)[0])
    drive = (process / "mh_uidrive.log").read_text(encoding="utf-8", errors="replace")
    require("[script] COMPLETE" in drive, "target invocation did not complete")
    require(
        re.search(r"\[sel\] qol_highindex_selected cam=1728,128 .*?\bn=0\b.*?\bbldg=30\b", drive),
        "wrong high-index receiver/camera",
    )
    require(
        re.search(r"\[sel\] qol_highindex_deselected .*?\bn=0\b.*?\bbldg=0\b", drive),
        "target cleanup did not actually deselect",
    )
    frames = [
        bigmap.load(str(process), "qol_highindex__" + name)
        for name in ("before", "rally", "phase", "deselected")
    ]
    native_size = (1920, 1088) if cropped else (2560, 1440)
    endpoint_index = 1705 if cropped else 2265
    require(
        all(frame is not None and frame.size == native_size for frame in frames),
        f"high-index proof needs four actual native {native_size!r} captures",
    )
    # Attribute the approved yellow rally palette against this same clean world,
    # including alpha coverage and subsequent native shadow darkening.
    pixels = [np.asarray(frame).astype(np.int16) for frame in frames]
    introduced = []
    for frame in pixels[1:3]:
        introduced.append(target_color_pixels(frame, pixels[0], (255, 224, 64)))
    endpoint = np.zeros(pixels[0].shape[:2], dtype=bool)
    endpoint[892:922, 806:826] = True
    corridor = np.zeros_like(endpoint)
    corridor[416:892, 100:850] = True
    # The old opaque-colour predicate rejected unrelated animated aircraft. Relative
    # alpha coverage also detects their changing channels: constrain the connector
    # assertion to its actual geometric band, not the whole world bounding box.
    yy, xx = np.indices(endpoint.shape)
    dx, dy = 816 - 208, 908 - 176
    corridor &= np.abs((xx - 208) * dy - (yy - 176) * dx) <= 12 * np.hypot(dx, dy)
    for mask in introduced:
        require(
            np.count_nonzero(mask & endpoint) >= 30,
            f"missing highlighted rally endpoint at dirty index{endpoint_index}",
        )
        require(
            np.count_nonzero(mask & corridor) >= 60,
            "missing connector beyond the old1018-cell binding",
        )
    require(
        np.count_nonzero((introduced[0] ^ introduced[1]) & corridor) >= 20,
        "high-index connector dashes did not animate",
    )
    strokes = introduced[0] | introduced[1]
    residue = strokes & (endpoint | corridor) & np.any(pixels[3] != pixels[0], axis=2)
    require(
        not np.any(residue),
        "selected target pixels survived deselection in the high-index dirty grid",
    )
    print(
        f"storage targets: PASS native{native_size[0]}x{native_size[1]}"
        f" endpoint-index{endpoint_index} animated high-index connector and clean deselection"
    )


def controls_scene(session):
    """Static geometry from THIS session's own recorded world."""
    import json

    path = session / "mh_rec_scene.bin"
    if not path.is_file():
        path = session / "mh_rec_scene.bin.gz"
    require(path.is_file(), "missing this session's recorded scene")
    blob = gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_bytes()
    require(len(blob) >= 96 and blob[:8] == b"MHWRLD\0\1", "invalid recorded scene header")
    fmt, fingerprint, count, length = struct.unpack_from("<4I", blob, 8)
    schema = json.loads((Path(__file__).parent / "data/world_snapshot_schema.json").read_text())
    require(
        fmt == mp.world_format()
        and fingerprint == int(mp.world_schema_fingerprint(schema, fmt), 16)
        and count == len(schema["blocks"]),
        "recorded scene schema is stale",
    )
    end = 96 + length
    require(end <= len(blob), "truncated recorded scene payload")
    blocks, offset = {}, 96
    for block in schema["blocks"]:
        require(offset + 8 <= end, "truncated scene block header")
        rid, size = struct.unpack_from("<II", blob, offset)
        require(
            rid == block["rid"] and offset + 8 + size <= end,
            "invalid scene block " + block["id"],
        )
        blocks[block["id"]] = blob[offset + 8 : offset + 8 + size]
        offset += 8 + size
    require(offset == end, "scene payload has unaccounted bytes")
    for name, size in (
        ("BUILDING", 100 * 0x842),
        ("STRAT_FACING_STEP_SIGN", 25 * 8),
    ):
        require(len(blocks[name]) == size, "changed static geometry extent " + name)
    return blocks


def controls_capture_clocks(process):
    text = (process / "mh_capture.log").read_text(encoding="utf-8", errors="replace")
    clocks = {}
    for match in re.finditer(
        r"capture_(target_controls_\S+)__b\.bmp written "
        r"\(presenter output 1024x768\) game_clock=([0-9A-Fa-f]{16})",
        text,
    ):
        name, bits = match.groups()
        require(name not in clocks, "duplicate actual presenter write " + name)
        clock = struct.unpack(">d", bytes.fromhex(bits))[0]
        require(clock == clock and abs(clock) < 1e12, "invalid presenter-write game clock")
        clocks[name] = clock
    return clocks


def controls_stems(np, sources, ends):
    """Exclusive motion envelopes for members and their independently measured centroid."""
    yy, xx = np.indices((768, 1024))
    envelopes, excluded = [], np.zeros((768, 1024), dtype=bool)
    for points in sources:
        for x, y in points:
            excluded |= (xx - x) ** 2 + (yy - y) ** 2 <= 48**2
    for x, y, radius in ends:
        excluded |= (xx - x) ** 2 + (yy - y) ** 2 <= radius**2
    for points, (ex, ey, _) in zip(sources, ends):
        envelope = np.zeros_like(excluded)
        for sx, sy in points:
            dx, dy = ex - sx, ey - sy
            length2 = dx * dx + dy * dy
            require(length2 > 0, "degenerate source-to-target corridor")
            along = ((xx - sx) * dx + (yy - sy) * dy) / length2
            distance = np.abs((xx - sx) * dy - (yy - sy) * dx) / length2**0.5
            envelope |= (along > 0) & (along < 1) & (distance <= 5)
        envelopes.append(envelope)

    # Leave a guard band around the other envelope, including arrow wings.
    def expand(mask):
        out = mask.copy()
        for dy in range(-5, 6):
            for dx in range(-5, 6):
                sy = slice(max(0, -dy), min(768, 768 - dy))
                sx = slice(max(0, -dx), min(1024, 1024 - dx))
                out[max(0, dy) : min(768, 768 + dy), max(0, dx) : min(1024, 1024 + dx)] |= mask[
                    sy, sx
                ]
        return out

    world = (xx >= 365) & (xx < 850) & (yy >= 32) & (yy < 720)
    return [
        envelopes[i]
        & ~np.logical_or.reduce(
            [expand(envelope) for j, envelope in enumerate(envelopes) if j != i]
        )
        & ~excluded
        & world
        for i in range(len(envelopes))
    ]


def check_controls(target, ordinal):
    import bisect
    import numpy as np

    rec, drive = recording(target, ordinal)
    directories = _rundir.process_and_sessions(target)
    process, session = Path(directories[0]), Path(directories[ordinal])
    scene = controls_scene(session)
    captures = controls_capture_clocks(process)

    def roster(label, expected):
        matches = re.findall(
            rf"\[sel\] {re.escape(label)} cam=(\d+),(\d+) "
            rf".*?\bn=(\d+)\b.*?\bbldg=0\b.*?\bids=([0-9,]*)",
            drive,
        )
        require(len(matches) == 1, "missing or duplicated selection " + label)
        x, y, count, ids = matches[0]
        require(
            (int(x), int(y)) == (1440, 1696)
            and int(count) == len(expected)
            and tuple(map(int, ids.split(","))) == expected,
            "wrong real roster/camera for " + label,
        )

    for label, ids in (
        ("target_unit5", (5,)),
        ("target_unit2", (2,)),
        ("target_pair", (2, 5)),
        ("target_controls_selected", (2, 5)),
        ("attack_common_selected", (2, 5)),
        ("attack_individual5_selected", (5,)),
        ("attack_individual2_selected", (2,)),
        ("attack_individual_selected", (2, 5)),
        ("attack_distinct5_selected", (5,)),
        ("attack_distinct2_selected", (2,)),
        ("attack_distinct_selected", (2, 5)),
    ):
        roster(label, ids)

    view = View(rec)
    times, samples, fresh = [], [], []
    previous = set()
    queue_at = rec.region_offset[rec.region_index["order_queue"]]
    count_at = rec.region_offset[rec.region_index["order_queue_count"]]
    clock_at = rec.region_offset[rec.region_index["game_clock"]]
    capacity = rec.regions[rec.region_index["order_queue"]].len // 0x44
    building_layout = view.layouts[sr.REGION_LAYOUTS["buildings"]["struct"]]
    building_bytes = rec.regions[rec.region_index["buildings"]].len
    require(building_bytes % (8 * building_layout.size) == 0, "invalid building row pitch")
    building_row = building_bytes // (8 * building_layout.size)

    def building(bid):
        slot = building_row + bid  # actual player1 row, not stock100 guessed into a raised row
        proto = view.field("buildings", slot, "building_id")
        require(0 < proto < 100, "missing live target building type")
        require(
            view.field("buildings", slot, "energy", "<d") > 0
            and view.field("buildings", slot, "state") != 2,
            "requested target building is not live",
        )
        # Generated cfg_final_struct_Building prefix:
        # int id; ushort invention,ai_build; byte type,height,width.
        height, width = struct.unpack_from("<BB", scene["BUILDING"], proto * 0x842 + 9)
        x, y = (view.field("buildings", slot, name) for name in ("x", "y"))
        require(
            0 < width <= 5 and 0 < height <= 5 and x < 64 and y < 64, "invalid target footprint"
        )
        mapped = visible = False
        for dx in range(width):
            for dy in range(height):
                tile = (((x + dx) % 64) << 8) | ((y + dy) % 64)
                mapped |= (
                    view.field("tile_objects", tile, "class_owner") == 0x41
                    and view.field("tile_objects", tile, "building") == bid
                )
                visible |= view.field("tile_objects", tile, "visibility") != 0
        require(mapped and visible, "target building is unmapped or invisible")
        return (
            (x * 32 + width * 16 - 1440) % 2048,
            (y * 32 + height * 16 - 1696) % 2048,
            max(width, height) * 16 + 16,
        )

    def source(uid):
        u = view.unit(uid)
        tile = (u["x"] << 8) | u["y"]
        require(
            view.field("tile_objects", tile, "class_owner") == 0x80
            and view.field("tile_objects", tile, "building") == uid,
            "controls fixture is no longer the two mapped ground actors",
        )
        facing = view.field("units", uid, "facing_target")
        micro = view.field("units", uid, "move_microstep", "<i")
        require(0 <= facing <= 24 and 0 <= micro <= 31, "invalid live ground interpolation")
        dx, dy = struct.unpack_from("<ii", scene["STRAT_FACING_STEP_SIGN"], facing * 8)
        require(dx in (-1, 0, 1) and dy in (-1, 0, 1), "invalid recorded facing signs")
        point = (
            (u["x"] * 32 + dx * (31 - micro) + 16 - 1440) % 2048,
            (u["y"] * 32 + dy * (31 - micro) + 16 - 1696) % 2048,
        )
        require(0 <= point[0] < 864 and 0 <= point[1] < 736, "actor left controls viewport")
        return point

    for step in view.frames():
        clock = struct.unpack_from("<d", view.buf, clock_at)[0]
        require(not times or clock >= times[-1], "recorded game clock moved backwards")
        times.append(clock)
        units = {}
        for uid in (2, 5):
            units[uid] = view.unit(uid)
            units[uid].update(
                {
                    name: view.field("units", uid, name, "<i" if name == "path_cursor" else None)
                    for name in ("target_ref", "target_index", "path_cursor", "path_slot_id")
                }
            )
        # Delay geometry validation until its own capture bracket: startup states
        # need not already be the accepted command scene.
        samples.append((step, units))
        count = struct.unpack_from("<i", view.buf, count_at)[0]
        require(0 <= count <= capacity, "invalid recorded due queue")
        current = set()
        for index in range(count):
            start = queue_at + index * 0x44
            raw = bytes(view.buf[start : start + 0x44])
            q = order_stream.Order(step, raw)
            if q.unit_index not in (2, 5) or q.owner_and_kind & 15 or q.kind not in (0x20, 0x80):
                continue
            key = raw[8:]  # stale direct-local exec_time is not command identity
            current.add(key)
            if key not in previous:
                fresh.append((step, q, struct.unpack_from("<13i", raw, 16)))
        previous = current

    require(len(times) >= 3, "too few recorded states to bracket presenter output")

    def bracket(name):
        key = "target_controls_" + name
        require(key in captures, "missing actual clock-stamped presenter write " + key)
        clock = captures[key]
        # Recorded states are pre-body. Include the prior sample and the first
        # sample beyond this clock, even when an exact clock match exists.
        lo = bisect.bisect_left(times, clock) - 1
        hi = bisect.bisect_right(times, clock)
        require(0 <= lo < hi < len(times), "presenter write lacks both recorded-state neighbours")
        return lo, hi

    def image(name):
        result = bigmap.load(str(process), "target_controls_" + name + "__b")
        require(result is not None, "missing actual presenter pixels " + name)
        result = np.asarray(result).astype(np.int16)
        require(result.shape == (768, 1024, 3), "controls need native1024x768 presenter output")
        return result

    travel = {
        0x0A,
        0x0B,
        0x0C,
        0x0F,
        0x10,
        0x11,
        0x12,
        0x15,
        0x16,
        0x18,
        0x21,
        0x27,
        0x2C,
        0x2D,
        0x30,
        0x36,
        0x37,
    }

    def accepted(u, bid):
        return (
            u["energy"] > 0
            and (u["target_ref"], u["target_index"]) == (0x41, bid)
            and (
                0x1A <= u["state"] <= 0x1C
                or (u["state"] in travel | {0x2E, 0x2F} and 0x1A <= u["order"] <= 0x1C)
            )
        )

    # The displayed endpoint is the latest actual common requested destination,
    # while live assigned formation slots remain independently checked below.
    cfg_layout = view.layouts["mh_cfg_final_struct_Unit"]
    require(
        cfg_layout.reliable and len(scene["UNIT"]) == 100 * cfg_layout.size,
        "invalid recorded unit-config table",
    )
    cfg_fields = {f.name: f for f in cfg_layout.fields if f.name in ("move_op_code", "move_op_arg")}
    require(
        len(cfg_fields) == 2 and all(f.size == 1 for f in cfg_fields.values()),
        "changed move-config field layout",
    )
    move_headers = {}
    for uid in (2, 5):
        proto = samples[0][1][uid]["unit_proto_id"]
        require(0 < proto < 100, "invalid selected unit prototype")
        move_headers[uid] = tuple(
            scene["UNIT"][proto * cfg_layout.size + cfg_fields[name].offset]
            for name in ("move_op_code", "move_op_arg")
        )

    def latest_common_move(step_limit):
        common_moves = {}
        for step, q, args in fresh:
            if step > step_limit or (q.param0, q.order_code) != move_headers[q.unit_index]:
                continue
            if any(value != -1 for value in args[2:12]):
                continue
            if (args[12] & 0xFFFFFF00) not in (0x4000, 0x8000) or not (args[12] & 255):
                continue
            key = (step, q.owner_and_kind, q.param0, q.order_code, *args[:2], args[12])
            common_moves.setdefault(key, set()).add(q.unit_index)
        accepted = [key for key, ids in common_moves.items() if ids == {2, 5}]
        require(accepted, "no genuine config-matched common move before this capture")
        return max(accepted)[4:6]

    requested_move = latest_common_move(samples[bracket("subgroups")[0]][0])

    def geometry(names, targets=None, shared=False, masks=True):
        lo = min(bracket(name)[0] for name in names)
        hi = max(bracket(name)[1] for name in names)
        wanted_steps = {samples[i][0] for i in range(lo, hi + 1)}
        sources = {2: set(), 5: set()}
        centres = set()
        ends = {}
        for step in view.frames():
            if step not in wanted_steps:
                continue
            frame_sources = {uid: source(uid) for uid in (2, 5)}
            a, b = frame_sources[2], frame_sources[5]
            require(
                abs(a[0] - b[0]) < 1024 and abs(a[1] - b[1]) < 1024,
                "fixture does not have a unique short-arc two-member centroid",
            )
            centres.add(((a[0] + b[0]) // 2, (a[1] + b[1]) // 2))
            for uid in (2, 5):
                u = view.unit(uid)
                if targets is None:
                    require(
                        u["order"] == 1
                        and u["state"] in (0x0A, 0x0C, 0x0F, 0x11, 0x12)
                        and view.field("units", uid, "path_cursor", "<i") >= 0
                        and view.field("units", uid, "path_slot_id") != 0xFF,
                        "real ground-path activity lost during capture bracket",
                    )
                    x, y = requested_move
                    end = ((x * 32 + 16 - 1440) % 2048, (y * 32 + 16 - 1696) % 2048, 18)
                else:
                    u["target_ref"] = view.field("units", uid, "target_ref")
                    u["target_index"] = view.field("units", uid, "target_index")
                    require(
                        accepted(u, targets[uid]),
                        f"unit{uid} never accepted/retained attack0x41:{targets[uid]} at step{step}",
                    )
                    end = building(targets[uid])
                require(uid not in ends or ends[uid] == end, "live endpoint moved during proof")
                ends[uid] = end
                sources[uid].add(frame_sources[uid])
        require(all(sources.values()), "empty accepted geometry bracket")
        # Default Subgroups source radius is2 tiles; prove these are genuinely
        # separate sources instead of assuming the existing positions stayed apart.
        require(
            min((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 for a in sources[2] for b in sources[5])
            > 64**2,
            "actors are within the default Subgroups source-join radius",
        )
        if not masks:
            return lo, hi, [], [ends[2], ends[5]], None
        corridors = controls_stems(np, [sources[2], sources[5]], [ends[2], ends[5]])
        shared_corridor = None
        if ends[2] == ends[5]:
            groups = controls_stems(
                np, [sources[2], sources[5], centres], [ends[2], ends[5], ends[2]]
            )
            shared_corridor = groups[2]
            if shared:
                corridors = groups[:2]
        return lo, hi, corridors, [ends[2], ends[5]], shared_corridor

    def attributable(frame, backgrounds, channel):
        result = np.ones(frame.shape[:2], dtype=bool)
        color = (64, 255, 96) if channel == 1 else (255, 80, 64)
        for background in backgrounds:
            result &= target_color_pixels(frame, background, color)
        # Remove independently changing artwork seen in the two zero-opacity
        # controls; it is not evidence of a target connector.
        result &= np.max(np.abs(backgrounds[0] - backgrounds[1]), axis=2) <= 4
        return result

    def stem_counts(mask, corridors, label):
        counts = []
        for corridor in corridors:
            require(
                np.count_nonzero(corridor) >= 180, label + ": insufficient exclusive connector area"
            )
            selected = mask & corridor
            counts.append(int(np.count_nonzero(selected)))
        return counts

    def strong(mask, corridor, label):
        ys, xs = np.nonzero(mask & corridor)
        require(len(xs) >= 24, f"{label}: only {len(xs)} attributable connector pixels; need24")
        require(
            len(set(zip(xs // 2, ys // 2))) >= 12,
            label + ": connector evidence is only a small pixel cluster",
        )

    move_names = (
        "subgroups",
        "shared",
        "opacity_zero",
        "minimum_0",
        "minimum_64",
        "minimum_restored",
        "minimum_zero",
    )
    _, move_hi, _, move_ends, _ = geometry(move_names, masks=False)
    backgrounds = [image("opacity_zero"), image("minimum_zero")]

    def logged_goals(label):
        matches = re.findall(rf"\[goal\] {re.escape(label)} n=2 goals=([^ ]+) side=0", drive)
        require(len(matches) == 1, "missing unique actual native goals for " + label)
        return {
            uid: (x, y)
            for uid, x, y in (tuple(map(int, item.split(":"))) for item in matches[0].split(","))
        }

    old_goals = logged_goals("target_controls_old_move")
    before_goals = logged_goals("target_controls_pending_before")
    after_goals = logged_goals("target_controls_pending_after")
    require(
        any(
            before_goals.get(uid) == after_goals.get(uid) == old_goals[uid]
            and old_goals[uid] != requested_move
            for uid in (2, 5)
        ),
        "immediate capture did not bracket an actually deferred older native goal",
    )
    pending_pixels = attributable(image("pending"), backgrounds, 1)
    px, py, _ = move_ends[0]
    pyy, pxx = np.indices((768, 1024))
    requested_marker = (np.abs(pxx - px) <= 10) & (np.abs(pyy - py) <= 10)
    require(
        np.count_nonzero(pending_pixels & requested_marker) >= 12,
        "new requested endpoint is absent while native goals still belong to the old move",
    )
    for gx, gy in set(old_goals.values()):
        ox, oy = (gx * 32 + 16 - 1440) % 2048, (gy * 32 + 16 - 1696) % 2048
        require(
            (ox - px) ** 2 + (oy - py) ** 2 >= 48**2,
            "retarget fixture does not separate old and requested markers",
        )
        old_marker = (np.abs(pxx - ox) <= 10) & (np.abs(pyy - oy) <= 10)
        require(
            np.count_nonzero(pending_pixels & old_marker) <= 3,
            "older assigned endpoint survived the newer admitted move",
        )
    print("  immediate retarget: PASS new requested marker before old native goals change")

    move_masks = {
        name: attributable(image(name), backgrounds, 1)
        for name in ("subgroups", "shared", "minimum_0", "minimum_64", "minimum_restored")
    }
    yy, xx = np.indices((768, 1024))
    endpoints = [
        (np.abs(xx - x) <= 10) & (np.abs(yy - y) <= 10) for x, y, _ in dict.fromkeys(move_ends)
    ]
    endpoint_counts = {
        name: [int(np.count_nonzero(mask & endpoint)) for endpoint in endpoints]
        for name, mask in move_masks.items()
    }
    require(
        all(n >= 30 for n in endpoint_counts["subgroups"]),
        "Subgroups lost the actual requested move endpoint",
    )
    full_move = image("subgroups")
    gain = np.sum((full_move - backgrounds[0]) * np.asarray((0.2126, 0.7152, 0.0722)), axis=2)
    bright = move_masks["subgroups"] & (full_move[:, :, 1] >= 200) & (gain >= 15)
    require(
        all(np.count_nonzero(bright & endpoint) >= 6 for endpoint in endpoints),
        "full-opacity movement markers remain dark against their actual terrain",
    )
    zero = image("opacity_zero")
    zero_green = (zero[:, :, 1] > zero[:, :, 0] + 12) & (zero[:, :, 1] > zero[:, :, 2] + 12)
    require(
        all(np.count_nonzero(zero_green & endpoint) == 0 for endpoint in endpoints),
        "zero opacity left movement artwork at the real endpoints",
    )
    _, _, shared_members, _, shared_centroid = geometry(["shared"], shared=True)
    require(shared_centroid is not None, "common move has no measurable centroid corridor")
    strong(move_masks["shared"], shared_centroid, "move/Shared centroid")
    require(
        all(n <= 3 for n in stem_counts(move_masks["shared"], shared_members, "move/Shared")),
        "Shared command retained a member-origin connector instead of its centroid",
    )
    for name in ("minimum_0", "minimum_restored"):
        _, _, own_corridors, _, _ = geometry([name])
        stem_counts(move_masks[name], own_corridors, name)
        for corridor in own_corridors:
            strong(move_masks[name], corridor, name)
    _, _, cutoff_members, _, cutoff_centroid = geometry(["minimum_64"])
    require(cutoff_centroid is not None, "cutoff scene lost its common destination")
    require(
        all(
            n <= 3
            for n in stem_counts(
                move_masks["minimum_64"],
                [np.logical_or.reduce([*cutoff_members, cutoff_centroid])],
                "minimum64",
            )
        ),
        "cutoff64 retained a connector",
    )
    # A marker-only cross has thirteen core pixels, not a stem's thirty-plus.
    require(
        all(n >= 6 for n in endpoint_counts["minimum_64"]),
        "cutoff64 suppressed an endpoint instead of only its stem",
    )

    # Derive zoomed endpoint/corridor from this invocation's actual input crop,
    # camera, command receipt and recorded member motion, not screenshot guesses.
    zoom_names = ("zoom_point", "zoom_area", "zoom_zero")
    zoom_lo = min(bracket(name)[0] for name in zoom_names)
    zoom_hi = max(bracket(name)[1] for name in zoom_names)
    zoom_goal = latest_common_move(samples[zoom_lo][0])
    camera = re.findall(r"\[sel\] target_zoom_scene cam=(\d+),(\d+) .*?n=2 bldg=0 ids=2,5", drive)
    require(len(camera) == 1, "missing actual zoom camera/selection")
    cam_x, cam_y = map(int, camera[0])
    zoom_log = (session / "mh_input.log").read_text(encoding="utf-8", errors="replace")
    zoom = re.search(
        r"\[zoom\] wheel in\s+z 1\.00 -> ([0-9.]+).*?origin \d+,\d+ -> (\d+),(\d+) "
        r"crop (\d+)x(\d+).*?view=(\d+)x(\d+)",
        zoom_log,
    )
    require(zoom is not None and float(zoom[1]) >= 1.1, "missing actual nonidentity zoom crop")
    left, top, crop_w, crop_h, view_w, view_h = map(int, zoom.groups()[1:])
    require(
        (view_w, view_h) == (864, 736) and 0 < crop_w < view_w and 0 < crop_h < view_h,
        "invalid native zoom viewport",
    )
    ex_native = (zoom_goal[0] * 32 + 16 - cam_x) % 2048
    ey_native = (zoom_goal[1] * 32 + 16 - cam_y) % 2048
    ex = (ex_native - left + 0.5) * view_w / crop_w - 0.5
    ey = (ey_native - top + 0.5) * view_h / crop_h - 0.5
    require(385 < ex < 830 and 25 < ey < 700, "zoom target is occluded by UI or outside crop")
    endpoint = (np.abs(xx - ex) <= 16) & (np.abs(yy - ey) <= 16)
    corridor = np.zeros((768, 1024), dtype=bool)
    wanted_steps = {samples[i][0] for i in range(zoom_lo, zoom_hi + 1)}
    for step in view.frames():
        if step not in wanted_steps:
            continue
        a, b = source(2), source(5)
        sx_native = (((a[0] + b[0]) // 2) + 1440 - cam_x) % 2048
        sy_native = (((a[1] + b[1]) // 2) + 1696 - cam_y) % 2048
        sx = (sx_native - left + 0.5) * view_w / crop_w - 0.5
        sy = (sy_native - top + 0.5) * view_h / crop_h - 0.5
        dx, dy = ex - sx, ey - sy
        length2 = dx * dx + dy * dy
        require(length2 > 100**2, "zoom connector has no independently measurable length")
        along = ((xx - sx) * dx + (yy - sy) * dy) / length2
        distance = np.abs((xx - sx) * dy - (yy - sy) * dx) / length2**0.5
        corridor |= (along > 0.1) & (along < 0.9) & (distance <= 7)
    corridor &= (xx >= 365) & (xx < 850) & (yy >= 32) & (yy < 720)
    zoom_background = image("zoom_zero")
    for name in ("zoom_point", "zoom_area"):
        pixels = target_color_pixels(image(name), zoom_background, (64, 255, 96))
        require(
            np.count_nonzero(pixels & endpoint) >= 8,
            name + ": requested marker was consumed by zoom filtering",
        )
        strong(pixels, corridor, name + "/actual centroid corridor")
    print(f"  zoom: PASS Point/Area requested endpoint({ex:.1f},{ey:.1f}) and centroid connector")

    # The same short move: pan only its destination outside the actual zoom
    # crop, not outside the backing RGB565 surface, with cutoff64 still set.
    cutoff_cameras = re.findall(
        r"\[sel\] target_cutoff_outside cam=(\d+),(\d+) .*?n=2 bldg=0 ids=2,5", drive
    )
    require(len(cutoff_cameras) == 1, "missing actual off-screen cutoff camera/selection")
    outside_camera = tuple(map(int, cutoff_cameras[0]))
    for name, camera, background in (
        ("cutoff_inside", (cam_x, cam_y), zoom_background),
        ("cutoff_outside", outside_camera, image("cutoff_outside_zero")),
        ("cutoff_inside_restored", (cam_x, cam_y), zoom_background),
    ):
        lo, hi = bracket(name)
        require(
            latest_common_move(samples[lo][0]) == zoom_goal, "cutoff proof changed its actual order"
        )
        cx, cy = camera
        nx = (zoom_goal[0] * 32 + 16 - cx) % 2048
        ny = (zoom_goal[1] * 32 + 16 - cy) % 2048
        inside = left <= nx < left + crop_w and top <= ny < top + crop_h
        require(
            inside == (name != "cutoff_outside"), name + ": destination has wrong crop visibility"
        )
        require(0 <= nx < view_w and 0 <= ny < view_h, name + ": target is outside backing surface")
        ex = (nx - left + 0.5) * view_w / crop_w - 0.5
        ey = (ny - top + 0.5) * view_h / crop_h - 0.5
        corridor = np.zeros((768, 1024), dtype=bool)
        wanted_steps = {samples[i][0] for i in range(lo, hi + 1)}
        for step in view.frames():
            if step not in wanted_steps:
                continue
            a, b = source(2), source(5)
            sx_native = (((a[0] + b[0]) // 2) + 1440 - cx) % 2048
            sy_native = (((a[1] + b[1]) // 2) + 1696 - cy) % 2048
            sx = (sx_native - left + 0.5) * view_w / crop_w - 0.5
            sy = (sy_native - top + 0.5) * view_h / crop_h - 0.5
            dx, dy = ex - sx, ey - sy
            length2 = dx * dx + dy * dy
            require(length2 > 100**2, name + ": connector is too short for attributable proof")
            along = ((xx - sx) * dx + (yy - sy) * dy) / length2
            distance = np.abs((xx - sx) * dy - (yy - sy) * dx) / length2**0.5
            corridor |= (along > 0.1) & (along < 0.9) & (distance <= 7)
        corridor &= (xx >= 365) & (xx < 850) & (yy >= 4) & (yy < 720)
        pixels = target_color_pixels(image(name), background, (64, 255, 96))
        if inside:
            require(
                np.count_nonzero(pixels & corridor) <= 3,
                name + ": on-screen cutoff retained a connector",
            )
        else:
            strong(pixels, corridor, name + "/clipped centroid connector")
    print(
        "  cutoff: PASS same zoomed short move inside -> off-screen -> inside, cutoff64 unchanged"
    )

    def attack_receipts(lo, hi):
        return [
            (step, q.unit_index, args[5], args[6])
            for step, q, args in fresh
            if lo < step <= hi and (0x1A <= q.param0 <= 0x1C or 0x1A <= q.order_code <= 0x1C)
        ]

    def one_receipt(start, finish, uid, bid):
        records = attack_receipts(samples[start][0], samples[finish][0])
        require(
            records and all(r[1:] == (uid, 0x41, bid) for r in records),
            f"missing exclusive real unit{uid} attack receipt for0x41:{bid}",
        )

    attack_ranges = {}
    for phase, targets in (
        ("common", {2: 1, 5: 1}),
        ("individual", {2: 1, 5: 1}),
        ("distinct", {2: 3, 5: 1}),
    ):
        prefix = "attack_" + phase + "_"
        names = [prefix + suffix for suffix in ("zero", "subgroups", "shared", "zero_after")]
        lo, hi, _, _, _ = geometry(names, targets, masks=False)
        attack_ranges[phase] = (lo, hi)
        backgrounds = [image(prefix + "zero"), image(prefix + "zero_after")]
        subgroups = attributable(image(prefix + "subgroups"), backgrounds, 0)
        shared = attributable(image(prefix + "shared"), backgrounds, 0)
        _, _, subgroup_corridors, _, _ = geometry([prefix + "subgroups"], targets)
        _, _, shared_corridors, _, shared_centroid = geometry(
            [prefix + "shared"], targets, shared=True
        )
        stem_counts(subgroups, subgroup_corridors, phase + "/Subgroups")
        shared_counts = stem_counts(shared, shared_corridors, phase + "/Shared")
        print(f"  attack {phase}: exclusive Shared pixels={shared_counts}")
        for corridor in subgroup_corridors:
            strong(subgroups, corridor, phase + "/Subgroups")
        if phase == "common":
            require(
                shared_centroid is not None, "common attack has no measurable centroid corridor"
            )
            require(
                all(n <= 3 for n in shared_counts),
                "Shared common attack retained a member-origin connector",
            )
            strong(shared, shared_centroid, "common/Shared centroid")
        else:
            for corridor in shared_corridors:
                strong(shared, corridor, phase + "/Shared")

    _, _, color_corridors, _, _ = geometry(
        ("attack_distinct_zero_after", "attack_color", "attack_color_zero"), {2: 3, 5: 1}
    )
    colored = image("attack_color")
    before = image("attack_distinct_zero_after")
    after = image("attack_color_zero")
    color_mask = target_color_pixels(colored, before, (255, 80, 255))
    color_mask &= target_color_pixels(colored, after, (255, 80, 255))
    color_mask &= np.max(np.abs(before - after), axis=2) <= 4
    gain = colored - before
    # The blue edit must recolor real connectors, not merely the editor swatch.
    color_mask &= (gain[:, :, 2] >= 20) & (gain[:, :, 2] >= gain[:, :, 0] * 0.65)
    for corridor in color_corridors:
        strong(color_mask, corridor, "Attack RGB editor")
    print("  attack RGB editor: PASS actual blue-gain connectors")

    # Common attack receipts must coexist as one real queued pair. This is
    # stronger than finding two units that happened to acquire the same target.
    common_lo, common_hi = attack_ranges["common"]
    grouped = {}
    for step, uid, ref, bid in attack_receipts(samples[move_hi][0], samples[common_lo][0]):
        require((ref, bid) == (0x41, 1), "common click did not issue target B")
        grouped.setdefault(step, set()).add(uid)
    require(
        any(ids == {2, 5} for ids in grouped.values()),
        "no actual paired common-attack receipt before common captures",
    )

    individual5_lo, individual5_hi = bracket("attack_individual5")
    individual_lo, individual_hi = attack_ranges["individual"]
    distinct5_lo, distinct5_hi = bracket("attack_distinct5")
    distinct_lo, _ = attack_ranges["distinct"]
    one_receipt(common_hi, individual5_lo, 5, 1)
    one_receipt(individual5_hi, individual_lo, 2, 1)
    one_receipt(individual_hi, distinct5_lo, 5, 1)
    one_receipt(distinct5_hi, distinct_lo, 2, 3)

    print(
        "target controls: PASS real common move; opacity-zero controls; "
        "cutoff0/64/restored with endpoints retained; common/individual/distinct attacks"
    )


def check_tactical(target, ordinal):
    import numpy as np

    rec, drive = recording(target, ordinal)
    directories = _rundir.process_and_sessions(target)
    process, session = Path(directories[0]), Path(directories[ordinal])
    harness = (process / "mh_harness.log").read_text(encoding="utf-8", errors="replace")
    entered = re.findall(
        r"PATH TACT step=(\d+) ENTER planet=(\d+) system=(\d+) squad=8 planet_save_rc=1",
        harness,
    )
    require(
        len(entered) == 1
        and len(re.findall(r"PATH TACT .* mission_start -> mode=6 .*active_units=8", harness)) == 1
        and len(re.findall(r"PATH TACT .* EXIT frames=30 mode=2", harness)) == 1,
        "missing one actual native tactical entry, mission and completed return",
    )
    entry_step, entry_planet, entry_system = map(int, entered[0])
    segments = (session / "mh_rec_segments.txt").read_text(encoding="utf-8", errors="replace")
    boundaries = re.findall(r"^segment \d+ step=(\d+) kind=tact_return\b", segments, re.M)
    require(len(boundaries) == 1, "ambiguous or missing actual tactical return boundary")
    boundary = int(boundaries[0])
    require(rec.first_step < boundary < rec.last_step - 20, "no continued post-return record")
    rid = rec.region_index["storage_policy"]
    base, length = rec.region_offset[rid], rec.regions[rid].len
    policy = {}
    view = View(rec)
    entry_world_seen = False
    for step in view.frames():
        if step == entry_step:
            routes = rec.region_offset[rec.region_index["order_tasks"]]
            system, planet, incarnation = struct.unpack_from("<3I", view.buf, routes + 24)
            require(
                (system, planet) == (entry_system, entry_planet) and incarnation > 0,
                "tactical fixture changed the strategic world identity before its save",
            )
            entry_world_seen = True
        if step in (boundary - 1, boundary):
            require(
                view.field("buildings", 30, "building_id") == 39,
                "tactical receiver identity is no longer the loaded Helipad",
            )
            policy[step] = bytes(view.buf[base : base + length])
    require(entry_world_seen, "missing recorded strategic world at tactical entry")
    require(
        policy.get(boundary - 1) == policy.get(boundary) and len(policy) == 2,
        "complete canonical policy changed across the actual tactical return",
    )
    incarnation, flags, x, y = struct.unpack_from("<4I", policy[boundary], 32 + 30 * 16)
    require(incarnation > 0 and (flags, x, y) == (7, 69, 16), "nondefault receiver was lost")
    for label in ("qol_tact_before", "qol_tact_reselected"):
        require(
            re.search(r"\[sel\] " + label + r" cam=1728,128 .*?\bn=0\b.*?\bbldg=30\b", drive),
            "wrong actual tactical receiver/camera at " + label,
        )
    frames = [
        bigmap.load(str(process), "qol_tact__" + name)
        for name in ("default", "before", "preserved")
    ]
    require(
        all(frame is not None and frame.size == (1024, 768) for frame in frames),
        "missing native tactical control frames",
    )
    off, on, restored = [np.asarray(frame)[276:294, 970:1024] for frame in frames]
    require(np.count_nonzero(np.any(off != on, axis=2)) >= 100, "Auto did not visibly latch")
    require(np.array_equal(on, restored), "latched Auto graphics were not restored")
    print(
        f"storage tactical: PASS real30-frame mission, full{length}-byte policy preserved at"
        f" boundary{boundary}, Auto pixels unchanged after return"
    )


def check_settings_resync(targets, ordinal, resync=True, recovery_steps=None):
    require(len(targets) == 2, "settings needs exactly host and client")
    if resync:
        require(
            recovery_steps is not None and recovery_steps > 0,
            "settings resync requires a positive --resync-within-steps allowance",
        )
    peers = [mp.load_peer(target) for target in targets]
    for index, peer in enumerate(peers):
        peer["role"] = mp.role_of(peer, "host" if index == 0 else "client")
        if peer.get("net_match"):
            peer["net"] = peer["net_match"]
    live = None
    windows = (("live settings", lambda step: True),)
    if resync:
        sync = [
            mp.parse_worldsync(peer.get("net_match") or peer.get("net") or []) for peer in peers
        ]
        require(len(sync[0]["begins"]) == 1, "expected exactly one deliberate host resync")
        require(len(sync[1]["lives"]) == 1, "expected exactly one actual client LIVE")
        live = sync[1]["lives"][0]["step"]
        require(live is not None and live > 3000, "LIVE must follow the delayed deliberate poke")
        generic = mp.resync_verify(peers, k=recovery_steps)
        require(mp.resync_report(generic), "the complete generic resync proof failed")
        windows = (
            ("before poke", lambda step: step < 3000),
            ("after LIVE", lambda step: step >= live + mp.RESYNC_MIN_TAIL_STEPS),
        )
    samples = []
    sessions = []
    records = []
    goal = None
    for peer_index, target in enumerate(targets):
        rec, drive = recording(target, ordinal)
        sessions.append(Path(_rundir.process_and_sessions(target)[ordinal]))
        records.append(rec)
        ready = re.findall(r"\[sel\] qol_mp_ready cam=(-?\d+),(-?\d+)", drive)
        if ready:
            require(len(ready) == 1, "ambiguous host storage selection")
        view = View(rec)
        rid = rec.region_index["storage_policy"]
        base = rec.region_offset[rid]
        length = rec.regions[rid].len
        rows = {}
        for step in view.frames():
            header = struct.unpack_from("<8I", view.buf, base)
            require(
                header[1] == 1 and header[7] == length, "invalid canonical policy schema/extent"
            )
            require(
                header[2] > 0 and header[3] > 2 and header[4] > 2, "missing fixture policy capacity"
            )
            if ready and goal is None:
                cx, cy = map(int, ready[0])
                goal = ((cx + 500) // 32 % header[5], (cy + 400) // 32 % header[6])
            incarnation, flags, x, y = struct.unpack_from("<4I", view.buf, base + 32 + 2 * 16)
            if not flags & 1:
                continue
            require(
                view.field("buildings", 2, "building_id") == 36, "fixture receiver is not barracks2"
            )
            require(incarnation != 0, "missing live receiver incarnation")
            if flags & 4:
                require(
                    view.field("units", 2, "state") == 0x1F and 2 in view.docked(1),
                    "Auto swept the existing docked occupant",
                )
            digest = hashlib.sha256(memoryview(view.buf)[base : base + length]).digest()
            rows[step] = (flags, incarnation, x, y, digest)
        require(rows, "no live receiver policy samples")
        require(
            {row[0] for row in rows.values()} >= {1, 3, 5, 7},
            "did not observe actual On, Rally, Off-during-transfer and self-clear states",
        )
        require(len({row[1] for row in rows.values()}) == 1, "receiver incarnation changed")
        if not resync or peer_index == 0:
            require(rows[max(rows)][0] == 1, "settings were not restored Off/cleared")
        samples.append(rows)
    require(goal is not None, "missing actual host camera/selection witness")
    for rows in samples:
        require(
            all((row[2], row[3]) == goal for row in rows.values() if row[0] & 2),
            "rally differs from the actual world click",
        )
    column = mp.REGION_NAMES.index("storage_policy")
    imported_digest = None
    if resync:
        excluded, keep_count = mp.resync_excluded_columns()
        require(
            keep_count is not None and "storage_policy" not in excluded,
            "policy must participate in the actual import and continued-state verdict",
        )
        capture = sync[0]["captures"][0]["S"]
        require(
            sync[1]["imports"] == [{"blobstep": capture, "rc": 0}],
            "missing successful import of this capture",
        )
        client_net = "\n".join(peers[1]["net"])
        stop = re.findall(r"\[worldsync\] IMPORT blobstep=(\d+) local=(\d+).*rc=0", client_net)
        require(
            stop == [(str(capture), str(records[1].last_step + 1))],
            "client prefix must end at the actual import discontinuity, not arbitrary truncation",
        )
        seed = sessions[1] / "mh_rec_seed_1.bin.gz"
        blob = gzip.decompress(seed.read_bytes())
        layout = mp.seed_region_layout(blob)
        spans = [(offset, length) for name, _, offset, length in layout if name == "storage_policy"]
        require(len(spans) == 1, "post-import segment seed lacks its canonical policy")
        offset, length = spans[0]
        imported = blob[offset : offset + length]
        incarnation, flags, x, y = struct.unpack_from("<4I", imported, 32 + 2 * 16)
        require(
            (incarnation, flags, x, y) == (samples[0][capture][1], 7, *goal),
            "actual imported seed lost the nondefault receiver identity/settings",
        )
        imported_digest = hashlib.sha256(imported).digest()
        require(
            imported_digest == samples[0][capture][-1],
            "post-import full policy bytes differ from the host's capture-era state",
        )
        # SNAPCAP/SNAPIMP are kind1; compare them only to each other, never to kind2 R rows.
        require(
            peers[0]["harness"]["snapcap"][capture][column]
            == peers[1]["harness"]["snapimp"][capture][column],
            "policy capture/import snapshot hashes differ",
        )
        transfers = []
        for session in sessions:
            path = session / "mh_match_orders.bin"
            unique = {}
            for q in order_stream.load(path):
                if (
                    capture < q.step <= records[1].last_step
                    and q.kind == 0
                    and q.order_code == q.param0 == 0xFD
                    and q.unit_index == 2
                    and q.owner_and_kind & 15 == 0
                ):
                    args = struct.unpack_from("<13i", q.raw, 16)
                    require(
                        args[:2] == (3, incarnation) and args[2] in (0, 1) and not any(args[3:]),
                        "malformed actual Auto setting payload",
                    )
                    unique.setdefault(q.raw, q)
            commands = sorted(unique.values(), key=lambda q: q.exec_time)
            require(
                [struct.unpack_from("<i", q.raw, 24)[0] for q in commands] == [0, 1],
                "missing ordered Off/On admissions during transfer",
            )
            transfers.append([q.raw for q in commands])
        require(transfers[0] == transfers[1], "transfer-time setting payloads differ between peers")
    common = sorted(set(samples[0]) & set(samples[1]))
    hash_steps = set(peers[0]["harness"]["regions"]) & set(peers[1]["harness"]["regions"])
    witnesses = []
    for label, predicate in windows:
        candidates = []
        for step in sorted(set(samples[0]) & hash_steps):
            if not predicate(step) or samples[0][step][0] != 7:
                continue
            if resync and step >= live:
                if samples[0][step][-1] != imported_digest:
                    continue
            elif step not in common or samples[0][step][-1] != samples[1][step][-1]:
                continue
            if (
                peers[0]["harness"]["regions"][step][column]
                == peers[1]["harness"]["regions"][step][column]
            ):
                candidates.append(step)
        require(candidates, "no nondefault full-policy and region-hash witness " + label)
        witnesses.append(candidates[0])
    if resync:
        cleared = [
            step
            for step in sorted(set(samples[0]) & hash_steps)
            if step >= live + mp.RESYNC_MIN_TAIL_STEPS
            and samples[0][step][:4] == (1, incarnation, 0, 0)
            and peers[0]["harness"]["regions"][step][column]
            == peers[1]["harness"]["regions"][step][column]
        ]
        require(cleared, "no actual post-LIVE cleared-policy witness on the client")
        witnesses.append(cleared[0])
    else:
        require(
            samples[0][max(samples[0])][-1] == samples[1][max(samples[1])][-1],
            "final cleared policy differs between peers",
        )
    print(f"storage settings: PASS goal={goal} policy+hash={witnesses} LIVE={live}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "target",
        nargs="+",
        help="exact invocation's process/session directories supplied by runner",
    )
    parser.add_argument(
        "--session", type=int, required=True, help="ordinal within THIS invocation, never newest"
    )
    parser.add_argument("--settings-resync", action="store_true")
    parser.add_argument(
        "--resync-within-steps",
        type=int,
        help="finite recovery allowance shared with the generic oracle",
    )
    parser.add_argument("--settings", action="store_true")
    parser.add_argument("--targets", action="store_true")
    parser.add_argument("--targets-cropped", action="store_true")
    parser.add_argument("--tactical", action="store_true")
    parser.add_argument("--controls", action="store_true")
    args = parser.parse_args()
    try:
        if args.controls:
            require(len(args.target) == 1, "controls needs exactly one invocation")
            check_controls(args.target[0], args.session)
        elif args.targets or args.targets_cropped or args.tactical:
            require(len(args.target) == 1, "targets needs exactly one invocation")
            check_targets(args.target[0], cropped=args.targets_cropped)
            if args.tactical:
                check_tactical(args.target[0], args.session)
        elif args.settings_resync or args.settings:
            check_settings_resync(
                args.target,
                args.session,
                resync=args.settings_resync,
                recovery_steps=args.resync_within_steps,
            )
        else:
            require(len(args.target) == 1, "repair needs exactly one invocation")
            check_repair(args.target[0], args.session)
    except (OSError, ValueError, KeyError, struct.error) as exc:
        print(f"storage repair: FAIL: {exc}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
