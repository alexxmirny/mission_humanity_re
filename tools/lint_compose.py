# lint_compose.py -- structural assertions over every tracked docker-compose file (dist RP4).
#
# WHAT THIS CATCHES, AND WHY IT IS A LINT ROW RATHER THAN A ONE-TIME REVIEW. RP4's own done_when
# says "the relay container runs with host networking (asserted by the compose file lint)" --
# plan D4's reason is real (Docker's userland proxy on the default bridge rewrites a UDP packet's
# apparent source address, which breaks the relay's connection-id demux outright), which makes
# `network_mode: host` on the relay service load-bearing correctness, not a convenience setting a
# future edit could "clean up" without anything else noticing. A YAML review at write time proves
# nothing about the NEXT edit -- this is what stays true across every future one. Three rules,
# over every `docker-compose.yml`/`compose.yml` this repo tracks:
#   1. any service that IS the relay (service key `relay`, or an `image:` naming `mh-relay`/
#      `mh_relay`) must set `network_mode: host` exactly.
#   2. any service that IS the collector (service key `collector`, or an `image:` naming
#      `mh-collector`) must NOT set `network_mode: host` -- the negative case RP4's scope line
#      states explicitly ("the collector does not").
#   3. no service, anywhere, may reference Watchtower (an `image:` under the `containrrr/`
#      or `nickfedor/` (its 2025 fork) namespaces, or a `com.centurylinklabs.watchtower.*`
#      label) -- RP4's scope line: "Watchtower is archived (Dec 2025) and is not used." Image
#      updates are pulled explicitly by .github/workflows/deploy.yml, not auto-updated in place.
#   4. (dist:RP7, 2026-09-20) the collector's REPORTS_DIR target (`/data/reports`) must be a BIND
#      mount whose host source is the drain root `/srv/reports` -- either the literal, or the
#      compose default-substitution form `${VAR:-/srv/reports}` (the dev stack's escape hatch for
#      a workstation without /srv). A named volume (`reports:/data/reports`) is exactly what the
#      first live stack had: uploads returned 201 into /var/lib/docker/volumes/..., the rrsync
#      drain key can only read /srv/reports, and six real reports were invisible for two days
#      (dead-ends G250). Both files are checked so the dev and prod collector blocks
#      cannot drift apart on this, and a missing /data/reports mount is a finding too.
#
# WHAT IT DOES NOT DO: this is not `docker compose config` (Docker itself is not installed on the
# machine this was authored on, 2026-09-17 -- see docs/deploy.md). It parses YAML and checks the
# FOUR structural facts above; it cannot tell you the compose file is otherwise runnable. Run
# `docker compose config` once Docker exists, as docs/deploy.md's verification section says.
# Whether the RUNNING container on the VPS actually has that bind is deploy.yml's job (its
# "collector writes the drain root" step inspects the live mount after every deploy).
#
# Run: python tools/lint_compose.py [--check] [--selftest]

import argparse
import os
import re
import sys

import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Directories that legitimately hold a compose file this lint should look at. NOT a bare
# repo-wide "**/docker-compose.yml" glob -- db/ is gitignored-but-present on a dev machine and a
# stray fixture directory should not silently gain lint coverage (or silently lose it) just by
# matching a filename; new compose-bearing directories are added here deliberately.
COMPOSE_DIRS = ["deploy", os.path.join("src", "collector")]
COMPOSE_NAMES = ("docker-compose.yml", "docker-compose.yaml", "compose.yml", "compose.yaml")

RELAY_IMAGE_HINTS = ("mh-relay", "mh_relay")
COLLECTOR_IMAGE_HINTS = ("mh-collector", "mh_collector")
WATCHTOWER_IMAGE_HINTS = ("containrrr/watchtower", "nickfedor/watchtower")
WATCHTOWER_LABEL_PREFIX = "com.centurylinklabs.watchtower."

# Rule 4: the collector's REPORTS_DIR inside the container, and the only host path allowed to back
# it -- the literal the drain key's `rrsync -ro /srv/reports` is pinned to (tools/drain_reports.py).
REPORTS_TARGET = "/data/reports"
DRAIN_ROOT = "/srv/reports"
_DEFAULT_SUBST = re.compile(r"^\$\{[A-Za-z_][A-Za-z0-9_]*:?-(?P<default>[^}]*)\}$")


def find_compose_files():
    out = []
    for d in COMPOSE_DIRS:
        for name in COMPOSE_NAMES:
            p = os.path.join(REPO, d, name)
            if os.path.isfile(p):
                out.append(p)
    return out


def _is_relay(service_name, svc):
    if service_name == "relay":
        return True
    image = str(svc.get("image", ""))
    return any(h in image for h in RELAY_IMAGE_HINTS)


def _is_collector(service_name, svc):
    if service_name == "collector":
        return True
    image = str(svc.get("image", ""))
    return any(h in image for h in COLLECTOR_IMAGE_HINTS)


def _reports_mount_source(svc):
    """The host-side source of the service's `/data/reports` mount, or None if there is no such
    mount. Understands both compose volume syntaxes: the short `src:dst[:opts]` string and the
    long `{type, source, target}` mapping (a long-form `type: volume` is reported as its volume
    name, so it fails rule 4 the same way a short-form name does)."""
    for entry in svc.get("volumes") or []:
        if isinstance(entry, str):
            # `${VAR:-default}` carries its own colon: peel a leading substitution off whole
            # before splitting `src:dst[:opts]`.
            if entry.startswith("${") and "}" in entry:
                src, _, rest = entry.partition("}")
                src += "}"
                rest = rest.removeprefix(":")
            else:
                src, _, rest = entry.partition(":")
            if rest.split(":")[0] == REPORTS_TARGET:
                return src
        elif isinstance(entry, dict) and entry.get("target") == REPORTS_TARGET:
            return str(entry.get("source", ""))
    return None


def _resolve_host_path(source):
    """`/srv/reports` -> itself; `${MH_REPORTS_DIR:-/srv/reports}` -> `/srv/reports`; a bare
    name (a named volume) or a `${VAR}` with no default -> None (nothing static to check)."""
    m = _DEFAULT_SUBST.match(source)
    if m:
        source = m.group("default")
    if source.startswith("/"):
        return source
    return None


def _labels_list(svc):
    labels = svc.get("labels")
    if labels is None:
        return []
    if isinstance(labels, dict):
        return ["%s=%s" % (k, v) for k, v in labels.items()]
    return [str(x) for x in labels]


def audit_compose(path, doc):
    """Return a list of finding strings for one parsed compose document. `path` is used only in
    the finding text, so the selftest can pass a synthetic path."""
    findings = []
    services = (doc or {}).get("services") or {}
    if not isinstance(services, dict):
        return ["%s: no 'services' mapping" % path]

    for name, svc in services.items():
        svc = svc or {}
        image = str(svc.get("image", ""))

        if any(h in image for h in WATCHTOWER_IMAGE_HINTS):
            findings.append(
                "%s: service %r uses a Watchtower image (%s) -- archived Dec 2025, not used "
                "(RP4 scope)" % (path, name, image)
            )
        for label in _labels_list(svc):
            if label.split("=", 1)[0].strip().startswith(WATCHTOWER_LABEL_PREFIX):
                findings.append(
                    "%s: service %r carries a Watchtower label (%s) -- not used (RP4 scope)"
                    % (path, name, label)
                )

        if _is_relay(name, svc):
            mode = svc.get("network_mode")
            if mode != "host":
                findings.append(
                    "%s: relay service %r has network_mode=%r, want 'host' (plan D4: the "
                    "default bridge's userland proxy rewrites the source address the "
                    "connection-id demux depends on)" % (path, name, mode)
                )

        if _is_collector(name, svc):
            mode = svc.get("network_mode")
            if mode == "host":
                findings.append(
                    "%s: collector service %r has network_mode=host -- it must stay on the "
                    "default bridge, reachable only through Caddy (RP4 scope: \"the collector "
                    "does not\")" % (path, name)
                )
            source = _reports_mount_source(svc)
            if source is None:
                findings.append(
                    "%s: collector service %r mounts nothing at %s -- reports would live inside "
                    "the container and die with it (RP7)" % (path, name, REPORTS_TARGET)
                )
            elif _resolve_host_path(source) != DRAIN_ROOT:
                findings.append(
                    "%s: collector service %r backs %s with %r, want a bind of the drain root "
                    "%s (or ${VAR:-%s}) -- anything else is invisible to the rrsync drain key "
                    "(RP7; dead-ends G250)"
                    % (path, name, REPORTS_TARGET, source, DRAIN_ROOT, DRAIN_ROOT)
                )

    return findings


def audit():
    findings = []
    files = find_compose_files()
    if not files:
        findings.append("no compose file found under %s" % ", ".join(COMPOSE_DIRS))
        return findings
    for path in files:
        with open(path, encoding="utf-8") as fh:
            try:
                doc = yaml.safe_load(fh)
            except yaml.YAMLError as e:
                findings.append("%s: invalid YAML (%s)" % (path, e))
                continue
        findings.extend(audit_compose(path, doc))
    return findings


# ---------------------------------------------------------------------------------------------
# selftest: a clean two-service fixture (relay host-networked, collector not) must report
# nothing; each mutation below must independently trigger exactly the finding it names.
# ---------------------------------------------------------------------------------------------


def _fixture():
    return {
        "services": {
            "relay": {"image": "ghcr.io/x/mh-relay:latest", "network_mode": "host"},
            "collector": {
                "image": "ghcr.io/x/mh-collector:latest",
                "volumes": ["/srv/reports:/data/reports"],
            },
            "caddy": {"image": "caddy:2-alpine"},
        }
    }


def _arm_relay_missing_host(doc):
    del doc["services"]["relay"]["network_mode"]


def _arm_relay_wrong_mode(doc):
    doc["services"]["relay"]["network_mode"] = "bridge"


def _arm_collector_gets_host(doc):
    doc["services"]["collector"]["network_mode"] = "host"


def _arm_watchtower_image(doc):
    doc["services"]["updater"] = {"image": "containrrr/watchtower:latest"}


def _arm_watchtower_label(doc):
    doc["services"]["relay"]["labels"] = {"com.centurylinklabs.watchtower.enable": "true"}


def _arm_reports_named_volume(doc):
    # The exact shape the first live stack shipped with (RP7).
    doc["services"]["collector"]["volumes"] = ["reports:/data/reports"]


def _arm_reports_wrong_bind(doc):
    doc["services"]["collector"]["volumes"] = ["/var/lib/mh/reports:/data/reports"]


def _arm_reports_wrong_default(doc):
    doc["services"]["collector"]["volumes"] = ["${MH_REPORTS_DIR:-./data/reports}:/data/reports"]


def _arm_reports_long_form_volume(doc):
    doc["services"]["collector"]["volumes"] = [
        {"type": "volume", "source": "reports", "target": "/data/reports"}
    ]


def _arm_reports_unmounted(doc):
    del doc["services"]["collector"]["volumes"]


ARMS = (
    ("relay missing network_mode: host", _arm_relay_missing_host),
    ("relay network_mode is not host", _arm_relay_wrong_mode),
    ("collector given network_mode: host", _arm_collector_gets_host),
    ("a Watchtower image", _arm_watchtower_image),
    ("a Watchtower label", _arm_watchtower_label),
    ("collector reports on a named volume", _arm_reports_named_volume),
    ("collector reports bound to a non-drain path", _arm_reports_wrong_bind),
    ("collector reports ${VAR:-default} with a non-drain default", _arm_reports_wrong_default),
    ("collector reports as a long-form named volume", _arm_reports_long_form_volume),
    ("collector with no /data/reports mount", _arm_reports_unmounted),
)


def selftest():
    import copy

    ok = True
    clean = audit_compose("fixture.yml", _fixture())
    if clean:
        print("SELFTEST: the clean fixture reports findings -- the lint over-refuses.")
        for f in clean:
            print("    " + f)
        ok = False
    # The dev stack's spelling of the same bind (rule 4's one permitted variant) must be clean too,
    # and so must the long-form bind -- both are legitimate ways to say "/srv/reports".
    for variant in (
        ["${MH_REPORTS_DIR:-/srv/reports}:/data/reports"],
        [{"type": "bind", "source": "/srv/reports", "target": "/data/reports"}],
    ):
        doc = copy.deepcopy(_fixture())
        doc["services"]["collector"]["volumes"] = variant
        got = audit_compose("fixture.yml", doc)
        if got:
            print("SELFTEST: the %r bind variant is refused -- the lint over-refuses." % (variant,))
            for f in got:
                print("    " + f)
            ok = False

    for label, mutate in ARMS:
        doc = copy.deepcopy(_fixture())
        mutate(doc)
        if not audit_compose("fixture.yml", doc):
            print("SELFTEST: the %r arm did NOT fire -- that failure would be invisible." % label)
            ok = False

    # The real files on disk must ALSO be clean -- a selftest that only ever exercises the
    # synthetic fixture would not notice deploy/docker-compose.yml itself regressing.
    real = audit()
    if real:
        print("SELFTEST: the real compose files on disk report findings:")
        for f in real:
            print("    " + f)
        ok = False

    print("compose lint selftest: %s (%d arms)" % ("ok" if ok else "FAILED", len(ARMS)))
    return ok


def main():
    ap = argparse.ArgumentParser(
        description="structural checks over every tracked docker-compose file (dist RP4, RP7)"
    )
    ap.add_argument("--check", action="store_true", help="the default action; kept for symmetry")
    ap.add_argument("--selftest", action="store_true", help="prove every arm still fires")
    args = ap.parse_args()

    if args.selftest:
        return 0 if selftest() else 1

    findings = audit()
    if findings:
        print("compose lint: %d problem(s)" % len(findings))
        for f in findings:
            print("  " + f)
        return 1
    print(
        "compose lint: %d file(s) ok (%s)"
        % (
            len(find_compose_files()),
            ", ".join(os.path.relpath(p, REPO) for p in find_compose_files()),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
