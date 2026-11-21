"""Check one real two-peer map download, without an RX discard or re-arm.

The transport writes JOIN, RX discard and READY to the process mh_net.log. The
map seam switches to the session log at JOIN, so both logs are collected.
"""

import argparse
import re

from check_content_refusal import collect_logs

STAMP = r"\[(\d\d):(\d\d):(\d\d)\.(\d{3})\]"


def events(text, pattern):
    rx = re.compile(STAMP + r"[^\n]*" + pattern)
    result = []
    for m in rx.finditer(text):
        h, minute, sec, ms = (int(v) for v in m.group(1, 2, 3, 4))
        result.append(((h * 3600 + minute * 60 + sec) * 1000 + ms, m.groups()[4:], m.start()))
    return result


def elapsed(first, second):
    return (second - first) % (24 * 60 * 60 * 1000)


def verdict(host, client):
    failures = []

    def one(text, pattern, label):
        found = events(text, pattern)
        if len(found) != 1:
            failures.append(f"{label}: expected once, got {len(found)}")
            return None
        return found[0]

    send = one(host, r"net: snapshot SEND armed -- (\d+) B .*root ([0-9a-f]+)", "snapshot SEND")
    one(host, r"; \[map\] send armed to peer ", "map SEND")
    start = one(host, r"; \[map\] start OK --", "Start gate opened")
    ready = one(client, r"net: snapshot READY -- (\d+) B .*root ([0-9a-f]+)", "snapshot READY")
    stored = one(
        client, r"; \[map\] client stored .*? \((\d+) B, sha=([0-9a-f]{16})\)", "map stored"
    )
    report = one(
        client,
        r"; \[map\] client reported the stored map to the host \(re-JOIN, sha=([0-9a-f]{16})\)",
        "completion re-JOIN",
    )
    joins = events(client, r"; S4 join: sent JOIN ")
    prep = events(client, r"; \[map\] client preparing JOIN --")
    if not joins or not prep:
        failures.append("fresh JOIN and its receive-side preparation must both be logged")
    elif elapsed(prep[0][0], joins[0][0]) > 55000 or (
        prep[0][0] == joins[0][0] and prep[0][2] >= joins[0][2]
    ):
        failures.append("receive-side preparation did not precede the fresh JOIN")
    if send and ready and send[1] != ready[1]:
        failures.append("snapshot SEND and READY differ in size or root")
    if ready and stored and ready[1][0] != stored[1][0]:
        failures.append("stored map length differs from the delivered snapshot")
    if stored and report and stored[1][1] != report[1][0]:
        failures.append("completion re-JOIN did not report the stored map")
    if send and start and elapsed(send[0], start[0]) > 55000:
        failures.append("host SEND to Start gate exceeded the existing 55-second scenario budget")
    if joins and ready:
        join = joins[0]
        duration = elapsed(join[0], ready[0])
        if duration > 55000 or (duration == 0 and ready[2] <= join[2]):
            failures.append(
                "client JOIN to READY exceeded the existing 55-second scenario budget or ran backwards"
            )
        # These three messages share the process log: seam_log writes the fresh JOIN
        # before opening the session; the UDP module's path stays at process scope.
        # At equal millisecond timestamps, physical line position breaks the tie.
        for discard in events(client, r"net: snapshot RX discarded --"):
            distance = elapsed(join[0], discard[0])
            after_join = distance > 0 or discard[2] > join[2]
            before_ready = distance < duration or (distance == duration and discard[2] < ready[2])
            if after_join and before_ready:
                failures.append("receive side was discarded after JOIN and before READY")
    if events(host, r"; \[map\] send timed out for peer "):
        failures.append("host re-armed after map send timeout")
    return failures


def selftest():
    def line(ms, text):
        return f"[12:00:00.{ms:03d}] {text}\n"

    send = line(
        11,
        "net: snapshot SEND armed -- 12 B in 1 body + 1 manifest chunk(s) to player 1, root 1234567890abcdef",
    )
    map_send = line(12, "; [map] send armed to peer 1")
    start = line(20, "; [map] start OK -- ready")
    discard = line(10, "net: snapshot RX discarded -- rewind")
    prep = line(10, "; [map] client preparing JOIN -- rewound")
    join = line(10, "; S4 join: sent JOIN client")
    ready = line(
        10, "net: snapshot READY -- 12 B in 1 chunk(s), root 1234567890abcdef, 0 re-request(s)"
    )
    stored = line(15, "; [map] client stored mh_dl/x.mpm (12 B, sha=fedcba0987654321)")
    report = line(
        16, "; [map] client reported the stored map to the host (re-JOIN, sha=fedcba0987654321)"
    )
    host = send + map_send + start
    client = discard + prep + join + ready + stored + report
    cases = [
        ("same-ms prepare rewind before JOIN", host, client, None),
        (
            "same-ms discard after JOIN",
            host,
            prep + join + discard + ready + stored + report,
            "receive side was discarded",
        ),
        (
            "same-ms discard after READY",
            host,
            prep + join + ready + discard + stored + report,
            None,
        ),
        (
            "preparation follows JOIN",
            host,
            join + prep + ready + stored + report,
            "preparation did not precede",
        ),
        ("READY precedes JOIN", host, prep + ready + join + stored + report, "ran backwards"),
        ("cache hit is not a transfer", "", prep + join, "snapshot SEND: expected once"),
        ("duplicate SEND", host + send, client, "snapshot SEND: expected once"),
        (
            "wrong root",
            host.replace("1234567890abcdef", "0123456789abcdef"),
            client,
            "differ in size or root",
        ),
        (
            "wrong size",
            host,
            client.replace("(12 B, sha=", "(13 B, sha="),
            "stored map length differs",
        ),
        (
            "wrong completion",
            host,
            client.replace("re-JOIN, sha=fedcba0987654321", "re-JOIN, sha=0000000000000000"),
            "did not report",
        ),
        (
            "host timeout",
            host + line(30, "; [map] send timed out for peer 1"),
            client,
            "host re-armed",
        ),
        (
            "client timeout",
            host,
            client.replace(
                "[12:00:00.010] net: snapshot READY", "[12:00:56.010] net: snapshot READY"
            ),
            "JOIN to READY exceeded",
        ),
    ]
    failed = 0
    for name, h, c, expected in cases:
        found = verdict(h, c)
        ok = not found if expected is None else any(expected in failure for failure in found)
        if not ok:
            print(f"FAIL {name}: {found}")
            failed += 1
    if not failed:
        print(f"PASS: {len(cases)} map-download oracle cases")
    return bool(failed)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("host", nargs="?")
    parser.add_argument("client", nargs="?")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.host or not args.client:
        parser.error("host and client run directories are required")
    failures = verdict(collect_logs(args.host), collect_logs(args.client))
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("PASS: one real map SEND/READY/store/re-JOIN and Start within the existing budget")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
