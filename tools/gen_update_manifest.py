#!/usr/bin/env python3
"""gen_update_manifest.py -- build and minisign-sign the `manifest.json` the launcher reads.

This is the publishing half of dist LA2. `tools/release_package.py` writes the three drop-in zips
and a `SHA256SUMS` into `dist/`; this reads that directory and emits the two files a launcher
fetches over the network:

    manifest.json           what exists, where it is, and what it hashes to
    manifest.json.minisig   a minisign signature over manifest.json, byte for byte

The launcher (`src/launcher/src/update.rs`) verifies the signature against ONE public key compiled
into its binary before it parses a single field, so a manifest this tool did not sign is not a
manifest the launcher will look at. dist LA3 calls this from the release workflow.

    python tools/gen_update_manifest.py --genkey --secret-key mh.key --public-key mh.pub
    python tools/gen_update_manifest.py --version 0.1.0 --dist dist --secret-key mh.key \\
        --asset-base-url https://github.com/<owner>/<repo>/releases/download/v0.1.0
    python tools/gen_update_manifest.py --verify dist/manifest.json [--public-key mh.pub]
    python tools/gen_update_manifest.py --sign-file FILE --secret-key mh.key [--trusted-comment T]
    python tools/gen_update_manifest.py --resign dist/manifest.json --secret-key mh.key --out DIR
    # schema 2 (RL8): one signed file per kind and channel, channels/<channel>/<kind>.json
    python tools/gen_update_manifest.py --kind game --channel latest --version 0.2.0 --dist dist \\
        --secret-key mh.key --asset-base-url https://github.com/<o>/<r>/releases/download/v0.2.0 \\
        --out pages/channels/latest [--min-launcher 0.2.0] [--channel-launcher launcher.json]
    python tools/gen_update_manifest.py --kind launcher --channel latest --version 0.2.0 \\
        --launcher-exe mh_launcher.exe --launcher-url <url> --secret-key mh.key --out DIR
    python tools/gen_update_manifest.py --promote latest/game.json --to stable --secret-key mh.key \\
        --out DIR
    python tools/gen_update_manifest.py --bridge --from-manifest old.json \\
        [--launcher-json stable/launcher.json] --secret-key mh.key --out DIR   # RL10 schema-1 bridge
    python tools/gen_update_manifest.py --age-days FILE   # verify against update.rs, print its age
    python tools/gen_update_manifest.py --selftest          # hermetic; a lint_repo row
    MH_RELAY_ADDR=host:port MH_RELAY_KEY=<64 hex> python tools/gen_update_manifest.py ...
                                                              # dist LA6: the relay the launcher
                                                              # provisions, inside the signed body

---- THE RELAY FIELD (dist LA6) -----------------------------------------------------------------

The manifest may carry `"relay": {"addr": "host:port", "key": "<64 hex digits>"}` beside the asset
table, INSIDE the signed body. The launcher writes `[net] transport=udp` + `[net] relay=<addr>`
into the game's mh_net.ini and `<key>` into mh_key.txt beside mh.exe, so a fresh install hosts
through the relay with no hand edit (src/launcher/src/relay.rs). Both values come from the release
workflow's repository settings at signing time -- the `MH_RELAY_ADDR` variable and the
`MH_RELAY_KEY` secret (`.github/workflows/release.yml`) -- via `--relay-addr`/`--relay-key` or the
environment variables of the same names; NEITHER may be written into this tree
(tools/lint_machine_paths.py refuses a public host, and machine_config.RELAY_KEY says why the key
stays out too). Given neither, the field is ABSENT and a launcher leaves the ini alone; given only
one, this tool REFUSES -- an address without its key is a relay nobody can authenticate to, and a
key without an address provisions nothing. The key is not a secret against players once shipped
(user decision 2026-09-19: it keeps scanners off the relay, not players), which is why it may
ride in a public manifest at all; the SIGNATURE is what stops anyone else from re-pointing every
launcher at a relay of their own.

---- WHY A MANIFEST AT ALL, WHEN GITHUB HAS AN API -----------------------------------------------

Plan decision D11, and it is a measurement rather than a preference. The unauthenticated GitHub REST
limit is 60 requests an hour PER IP, and a conditional request answered `304 Not Modified` still
spends one -- so a launcher that polls the Releases API rate-limits every player behind one NAT
together, and the polite ETag dance does not help. `releases/latest` additionally sorts by the
COMMIT date of the tagged commit rather than by publication date, so it can name a release that is
not the newest one. A static signed file on GitHub Pages has neither property: it is a CDN GET, it
is not rate limited, and it says what it says. The launcher therefore talks to Pages and to the
release asset CDN, and to `api.github.com` never -- which is a clause dist LA2 is accepted on and an
assertion in the launcher's own tests.

---- WHY THE CRYPTO IS IN THIS FILE INSTEAD OF A DEPENDENCY --------------------------------------

Signing is Ed25519 over BLAKE2b-512, both of which this file implements on the standard library
alone: `hashlib` already ships BLAKE2b and SHA-512, and the ~60 lines of Ed25519 below are RFC 8032's
own reference implementation. THAT IS A DELIBERATE CHOICE AND HERE IS THE CASE FOR IT.

  * There is no usable pure-Python minisign on PyPI. The `minisign` distribution is version 0.1.0,
    classified "Development Status :: 1 - Planning", and its own README's "Achieve basic
    functionality -- create key pair / verify signature / sign" checkboxes are all unticked.
  * The alternative is a libsodium binding (PyNaCl) or a `cargo install rsign2` in CI. Both are real
    dependencies for one 64-byte signature per release, and `tools/requirements.txt` is DERIVED from
    imports by `tools/lint_requirements.py`, so the pin is not free either.
  * The part of this scheme an attacker attacks is the VERIFIER, and the verifier is not here: it is
    the `minisign-verify` crate inside the launcher. This file only produces bytes that crate then
    has to accept. A signer that is wrong does not weaken anything -- it fails to publish.
  * And it is checked three ways: `--selftest` runs RFC 8032's published test vectors through the
    Ed25519 code, round-trips a key and a signature through this file's own verifier, and the
    launcher's `update::tests` verify a signature THIS TOOL PRODUCED with the real crate.

WHAT IS NOT IMPLEMENTED, stated rather than hidden: a PASSWORD-PROTECTED secret key. minisign
encrypts the secret key with scrypt by default (`-W` turns it off; `rsign generate -W` likewise),
and while `hashlib.scrypt` could do the KDF, libsodium's opslimit/memlimit -> (N, r, p) derivation
would have to be reproduced exactly and there is no `minisign` binary on this machine to check the
result against. An untested decryption path in a signing tool is worse than a refusal, so an
encrypted key is refused BY NAME with the flag that produces an unencrypted one. In CI the key is a
repository secret either way, and a passphrase would be a second secret guarding the first.

---- THE FILE FORMATS, so they can be read against the spec ---------------------------------------

minisign (https://jedisct1.github.io/minisign/), all little-endian, all base64 of a packed struct:

    public key      "Ed" | key_id[8] | pk[32]                                        = 42 B
    secret key      "Ed" | kdf_alg[2] | "B2" | salt[32] | ops[8] | mem[8]
                        | key_id[8] | sk[64] | chk[32]                               = 158 B
                    kdf_alg 0x0000 = not encrypted; chk = BLAKE2b-256("Ed"|key_id|sk)
    signature       line 1  untrusted comment: ...
                    line 2  base64("ED" | key_id[8] | sig[64])
                    line 3  trusted comment: ...
                    line 4  base64(global_sig[64])
                    sig        = Ed25519(sk, BLAKE2b-512(file))   -- "ED" is the PREHASHED algorithm
                    global_sig = Ed25519(sk, sig | trusted-comment bytes)

The trusted comment is signed and the untrusted one is not, which is why the version and the
timestamp go in the manifest (signed as file content) and nothing load-bearing goes in either
comment.
"""

import argparse
import base64
import hashlib
import io
import json
import os
import re
import secrets
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIST = os.path.join(REPO, "dist")

SCHEMA = 1  # the bridge manifest (RL10) and everything an rc7.1 launcher reads
SCHEMA2 = 2  # per-kind, per-channel files (RL8)
KINDS = ("game", "launcher")
CHANNELS = ("stable", "latest")
# The oldest launcher a game manifest built by THIS tool may require. Bump it in the commit that
# makes the game need a newer launcher; --channel-launcher then refuses a game release the served
# launcher cannot run, so "game first, launcher later" is a red job rather than a blocked player.
# "0.2.0-rc8", not "0.2.0": semver sorts a prerelease BELOW its release, so "0.2.0" would block every
# v0.2.0-rcN game on the rc launchers testers run (refuse LAUNCHER TOO OLD). rc8 is the first launcher
# that moves the ini, key and logs to user storage (dist RL4), which the rc8 game expects.
MIN_LAUNCHER = "0.2.0-rc8"
MANIFEST_NAME = "manifest.json"
SIGNATURE_NAME = "manifest.json.minisig"
SUMS_NAME = "SHA256SUMS"

# The configurations tools/release_package.py packages (v0.2.0 D1: ONE -- `net`). Stated here as
# well as there because this tool must refuse a dist/ that is missing one rather than publish a
# manifest with a hole in it, and importing the packager for one tuple would drag its whole module in.
TAGS = ("net",)
BASE_NAME = "mission_humanity_re"

# dist LA6: the two environment variables the release workflow hands the relay in by. Read by
# `main` as the default for `--relay-addr` / `--relay-key`, so a secret never has to appear on a
# command line (where a process listing would show it).
RELAY_ADDR_ENV = "MH_RELAY_ADDR"
RELAY_KEY_ENV = "MH_RELAY_KEY"


class Refusal(Exception):
    """A named reason to stop. Never a traceback: a publishing refusal is an ANSWER."""


def check_relay_addr(addr):
    """`host:port` as mh_net.ini's `[net] relay=` reads it (src/mh_dll/mh_net.example.ini). Refused
    rather than passed through: this value is written verbatim into a player's ini by the launcher,
    and a `;` in it would be read as a comment there (the R7a reader trims at the first `;`), a
    space would be a second token, and a port outside 1..65535 is not a port."""
    addr = (addr or "").strip()
    if not addr:
        raise Refusal("relay address is empty")
    if any(c.isspace() for c in addr) or ";" in addr or "#" in addr or "=" in addr:
        raise Refusal(
            "relay address %r may not contain whitespace, ';', '#' or '=' -- it is written into "
            "mh_net.ini as `relay=%s` verbatim" % (addr, addr)
        )
    host, sep, port = addr.rpartition(":")
    if not sep or not host or not port.isdigit() or not 1 <= int(port) <= 65535:
        raise Refusal("relay address %r is not host:port with a port in 1..65535" % addr)
    if host.startswith("[") != host.endswith("]"):
        raise Refusal("relay address %r has an unbalanced IPv6 bracket" % addr)
    return addr


def check_relay_key(key):
    """The relay deployment key as mh_key.txt holds it: 64 hex digits (MH_KEY_HEX_LEN) or the literal
    word `open`. Normalised to lower case so the launcher's "overwrite only if different" compares
    one spelling. Refused otherwise -- the game FAILS CLOSED on an unparseable key file
    (mh_net_key.h), so a typo here would be every player's game refusing to start."""
    key = (key or "").strip()
    if key.lower() == "open":
        return "open"
    if len(key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in key):
        raise Refusal(
            "relay key must be 64 hex digits (or the word open); got %d character(s)" % len(key)
        )
    return key.lower()


def relay_field(addr, key):
    """-> the `relay` object, or None when NEITHER was given. One without the other is a refusal,
    not a half-field: see the header."""
    addr = (addr or "").strip()
    key = (key or "").strip()
    if not addr and not key:
        return None
    if not addr:
        raise Refusal(
            "a relay key was given without a relay address (%s / --relay-addr) -- a key alone "
            "provisions nothing" % RELAY_ADDR_ENV
        )
    if not key:
        raise Refusal(
            "a relay address was given without its deployment key (%s / --relay-key) -- players "
            "could not authenticate to it" % RELAY_KEY_ENV
        )
    return {"addr": check_relay_addr(addr), "key": check_relay_key(key)}


# --------------------------------------------------------------------------- Ed25519 (RFC 8032)
#
# The reference implementation from RFC 8032 section 6, unchanged in substance. It is slow --
# roughly ten milliseconds per signature -- and a release signs exactly two things, so the constant
# factor is beneath notice.

_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493


def _inv(x):
    return pow(x, _P - 2, _P)


_D = -121665 * _inv(121666) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _sha512(b):
    return hashlib.sha512(b).digest()


def _sha512_modq(b):
    return int.from_bytes(_sha512(b), "little") % _Q


def _point_add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _point_mul(s, p):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _point_add(q, p)
        p = _point_add(p, p)
        s >>= 1
    return q


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * _inv(5) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _compress(p):
    zi = _inv(p[2])
    x, y = p[0] * zi % _P, p[1] * zi % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


def _expand(seed):
    h = _sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_public_key(seed):
    """The 32-byte public key for a 32-byte seed."""
    a, _ = _expand(seed)
    return _compress(_point_mul(a, _G))


def ed25519_sign(seed, msg):
    """A 64-byte detached signature over `msg`."""
    a, prefix = _expand(seed)
    pk = _compress(_point_mul(a, _G))
    r = _sha512_modq(prefix + msg)
    rs = _compress(_point_mul(r, _G))
    h = _sha512_modq(rs + pk + msg)
    return rs + int.to_bytes((r + h * a) % _Q, 32, "little")


def ed25519_verify(pk, msg, sig):
    """True when `sig` is a valid signature over `msg` under `pk`. Used by --selftest and --verify;
    the launcher has its own verifier and does not trust this one."""
    if len(sig) != 64 or len(pk) != 32:
        return False
    a = _decompress(pk)
    if a is None:
        return False
    r = _decompress(sig[:32])
    if r is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _Q:
        return False
    h = _sha512_modq(sig[:32] + pk + msg)
    sb = _point_mul(s, _G)
    rha = _point_add(r, _point_mul(h, a))
    # Compare projectively: x1/z1 == x2/z2 and y1/z1 == y2/z2.
    if (sb[0] * rha[2] - rha[0] * sb[2]) % _P != 0:
        return False
    return (sb[1] * rha[2] - rha[1] * sb[2]) % _P == 0


# --------------------------------------------------------------------------- minisign file formats

SIG_ALG_PREHASHED = b"ED"
SIG_ALG_LEGACY = b"Ed"
KDF_NONE = b"\x00\x00"
CKSUM_ALG = b"B2"


def _b64(raw):
    return base64.standard_b64encode(raw).decode("ascii")


def _unb64(text):
    try:
        return base64.standard_b64decode(text.strip().encode("ascii"))
    except Exception as exc:  # noqa: BLE001 -- the reason is the message
        raise Refusal("not valid base64: %s" % exc)


def _key_id_hex(key_id):
    """minisign prints the key id as the BIG-endian hex of a little-endian u64, i.e. the eight bytes
    reversed. Cosmetic -- it appears only in a comment -- but a file that prints it the other way
    round would not match what `minisign -R` shows for the same key."""
    return key_id[::-1].hex().upper()


def format_public_key(key_id, pk):
    return "untrusted comment: minisign public key %s\n%s\n" % (
        _key_id_hex(key_id),
        _b64(b"Ed" + key_id + pk),
    )


def format_secret_key(key_id, seed, pk):
    sk = seed + pk
    chk = hashlib.blake2b(b"Ed" + key_id + sk, digest_size=32).digest()
    packed = (
        b"Ed"
        + KDF_NONE
        + CKSUM_ALG
        + b"\x00" * 32  # kdf salt: unused with KDF_NONE, present because the struct has the field
        + b"\x00" * 8  # kdf opslimit
        + b"\x00" * 8  # kdf memlimit
        + key_id
        + sk
        + chk
    )
    assert len(packed) == 158, len(packed)
    return (
        "untrusted comment: minisign secret key (UNENCRYPTED -- this file is a secret)\n%s\n"
        % _b64(packed)
    )


def parse_secret_key(text):
    """-> (key_id, seed, pk). Refuses an encrypted key by name."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    # The `untrusted comment:` line is OPTIONAL. A key that reaches CI as a repository secret is
    # often pasted as its base64 line alone (v0.1.0-rc2, 2026-09-18: a 212-char one-line secret was
    # refused as "no base64 line" and the release lost its manifest); the comment carries nothing
    # the signature needs. What is refused is a file that is ONLY the comment, or empty.
    body = [ln for ln in lines if not ln.lstrip().lower().startswith("untrusted comment:")]
    if not body:
        raise Refusal("the secret key file has no base64 line")
    raw = _unb64(body[-1].strip())
    if len(raw) != 158:
        raise Refusal("a minisign secret key is 158 bytes packed, this one is %d" % len(raw))
    if raw[0:2] != b"Ed":
        raise Refusal("unsupported signature algorithm %r (expected Ed)" % raw[0:2])
    if raw[2:4] != KDF_NONE:
        raise Refusal(
            "ENCRYPTED SECRET KEY: this tool reads unencrypted keys only (kdf_alg %r).\n"
            "Generate one with `python tools/gen_update_manifest.py --genkey`, or with\n"
            "`minisign -G -W` / `rsign generate -W`. See this file's header for why the\n"
            "scrypt path is refused rather than guessed at." % raw[2:4]
        )
    key_id, sk, chk = raw[54:62], raw[62:126], raw[126:158]
    if hashlib.blake2b(b"Ed" + key_id + sk, digest_size=32).digest() != chk:
        raise Refusal("the secret key's checksum does not match -- the file is damaged")
    seed, pk = sk[:32], sk[32:]
    if ed25519_public_key(seed) != pk:
        raise Refusal("the secret key's embedded public key does not match its seed")
    return key_id, seed, pk


def parse_public_key(text):
    """-> (key_id, pk). Accepts either a whole .pub file or the bare base64 line."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        raise Refusal("the public key is empty")
    raw = _unb64(lines[-1])
    if len(raw) != 42:
        raise Refusal("a minisign public key is 42 bytes packed, this one is %d" % len(raw))
    if raw[0:2] not in (b"Ed", b"ED"):
        raise Refusal("unsupported signature algorithm %r" % raw[0:2])
    return raw[2:10], raw[10:42]


def public_key_line(key_id, pk):
    """The bare base64 line -- what gets compiled into the launcher."""
    return _b64(b"Ed" + key_id + pk)


def sign_bytes(key_id, seed, data, untrusted_comment, trusted_comment):
    """A prehashed ("ED") minisign signature over `data`, as the four-line .minisig text."""
    digest = hashlib.blake2b(data, digest_size=64).digest()
    sig = ed25519_sign(seed, digest)
    tc = trusted_comment.encode("utf-8")
    global_sig = ed25519_sign(seed, sig + tc)
    return "untrusted comment: %s\n%s\ntrusted comment: %s\n%s\n" % (
        untrusted_comment,
        _b64(SIG_ALG_PREHASHED + key_id + sig),
        trusted_comment,
        _b64(global_sig),
    )


def verify_signature(pk_text, data, sig_text):
    """The tool's own verifier: the round-trip check for --selftest and --verify. The launcher does
    NOT use this -- it uses the `minisign-verify` crate, which is the whole point."""
    key_id, pk = parse_public_key(pk_text)
    lines = sig_text.splitlines()
    if len(lines) < 4:
        raise Refusal("a .minisig has four lines, this one has %d" % len(lines))
    raw = _unb64(lines[1])
    if len(raw) != 74:
        raise Refusal("the signature line unpacks to %d bytes, expected 74" % len(raw))
    alg, sig_key_id, sig = raw[0:2], raw[2:10], raw[10:74]
    if sig_key_id != key_id:
        raise Refusal(
            "key id mismatch: the signature names %s, the public key is %s"
            % (_key_id_hex(sig_key_id), _key_id_hex(key_id))
        )
    if alg == SIG_ALG_PREHASHED:
        signed = hashlib.blake2b(data, digest_size=64).digest()
    elif alg == SIG_ALG_LEGACY:
        signed = data
    else:
        raise Refusal("unsupported signature algorithm %r" % alg)
    if not ed25519_verify(pk, signed, sig):
        raise Refusal("SIGNATURE DOES NOT VERIFY")
    if not lines[2].startswith("trusted comment: "):
        raise Refusal("line 3 is not a trusted comment")
    tc = lines[2][len("trusted comment: ") :].encode("utf-8")
    if not ed25519_verify(pk, sig + tc, _unb64(lines[3])):
        raise Refusal("the trusted comment's global signature does not verify")
    return True


# --------------------------------------------------------------------------- the manifest itself


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_sums(dist_dir):
    """`SHA256SUMS` as {name: digest}. It is the packager's own statement about what it built, so
    reading it -- rather than only re-hashing -- is what makes a mismatch between the two visible."""
    path = os.path.join(dist_dir, SUMS_NAME)
    if not os.path.isfile(path):
        raise Refusal("no %s in %s -- run tools/release_package.py first" % (SUMS_NAME, dist_dir))
    out = {}
    with io.open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            digest, name = line.split("  ", 1)
            out[name] = digest
        return out


def join_url(base, name):
    return base.rstrip("/") + "/" + name


def build_manifest(
    version,
    dist_dir,
    asset_base_url,
    launcher_version=None,
    launcher_exe=None,
    launcher_url=None,
    notes_url="",
    issued_at=None,
    relay_addr=None,
    relay_key=None,
):
    """The manifest as an ordered dict. Every digest is RE-HASHED off disk and then CHECKED against
    SHA256SUMS: the manifest must not be able to promise a hash the packager did not compute.
    `relay_addr`/`relay_key` (dist LA6) add the `relay` field, or nothing when both are empty."""
    relay = relay_field(relay_addr, relay_key)
    dist_dir = os.path.abspath(dist_dir)
    sums = read_sums(dist_dir)

    game = {}
    for tag in TAGS:
        name = "%s-%s-%s.zip" % (BASE_NAME, version, tag)
        path = os.path.join(dist_dir, name)
        if not os.path.isfile(path):
            raise Refusal(
                "MISSING ASSET: %s is not in %s.\n"
                "The manifest names every configuration or it names none: a launcher offered "
                "a partial set would show a player an update they cannot install."
                % (name, dist_dir)
            )
        digest = sha256(path)
        if name not in sums:
            raise Refusal("%s is on disk but not named in %s" % (name, SUMS_NAME))
        if sums[name] != digest:
            raise Refusal(
                "DIGEST MISMATCH: %s hashes to %s but %s says %s -- the dist directory holds a zip "
                "from a different packaging run" % (name, digest, SUMS_NAME, sums[name])
            )
        game[tag] = {
            "url": join_url(asset_base_url, name),
            "sha256": digest,
            "size": os.path.getsize(path),
        }

    if launcher_exe is None:
        raise Refusal("--launcher-exe is required: the manifest always carries a launcher entry")
    if not os.path.isfile(launcher_exe):
        raise Refusal("no launcher executable at %s" % launcher_exe)
    launcher = {
        "version": launcher_version or version,
        "url": launcher_url or join_url(asset_base_url, os.path.basename(launcher_exe)),
        "sha256": sha256(launcher_exe),
    }

    manifest = {
        "schema": SCHEMA,
        "version": version,
        "issued_at": issued_at or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "launcher": launcher,
        "game": game,
    }
    if relay is not None:
        # Beside the asset table and before notes_url, so a reader of the file meets it where the
        # launcher does. Absent -- not null, not empty -- when there is no relay: `Option<Relay>`
        # on the Rust side reads a missing key as None, and "no field" is the only spelling that
        # cannot be mistaken for a relay with blank values.
        manifest["relay"] = relay
    manifest["notes_url"] = notes_url
    return manifest


def manifest_bytes(manifest):
    """The exact bytes that get signed AND published. Serialised once, here, so the signature can
    never be over a different spelling of the same object: `sort_keys` is off because the field
    order above is the reading order, and the trailing newline is part of the signed content."""
    return (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def write_manifest(out_dir, manifest, key_id, seed, say=print):
    data = manifest_bytes(manifest)
    trusted = "mission humanity %s, issued %s" % (manifest["version"], manifest["issued_at"])
    sig = sign_bytes(
        key_id,
        seed,
        data,
        "signature from the mission humanity release key",
        trusted,
    )
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    mpath = os.path.join(out_dir, MANIFEST_NAME)
    spath = os.path.join(out_dir, SIGNATURE_NAME)
    with open(mpath, "wb") as fh:
        fh.write(data)
    with io.open(spath, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sig)
    say("  wrote      %-28s %d B" % (MANIFEST_NAME, len(data)))
    say("  wrote      %-28s signed by key %s" % (SIGNATURE_NAME, _key_id_hex(key_id)))
    return mpath, spath


# --------------------------------------------------------------------------- sign-file / resign

UPDATE_RS = os.path.join(REPO, "src", "launcher", "src", "update.rs")


def launcher_public_key(update_rs=UPDATE_RS):
    """The `PUBLIC_KEY` line compiled into the launcher, read from update.rs. The ONLY key a
    re-sign may be checked against: the key you happen to hold is not the key players trust."""
    with io.open(update_rs, encoding="utf-8") as fh:
        m = re.search(r'pub const PUBLIC_KEY: &str = "([A-Za-z0-9+/=]+)";', fh.read())
    if not m:
        raise Refusal("no `pub const PUBLIC_KEY` found in %s" % update_rs)
    return m.group(1)


def _now_rfc3339():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sign_file(path, key_id, seed, trusted_comment=None, out=None):
    """Minisign-sign any file with the release key -> the .minisig path. Same signer and key path
    as the manifest; the default trusted comment names the file and the time."""
    with open(path, "rb") as fh:
        data = fh.read()
    if trusted_comment is None:
        trusted_comment = "mission humanity file %s, signed %s" % (
            os.path.basename(path),
            _now_rfc3339(),
        )
    sig = sign_bytes(
        key_id, seed, data, "signature from the mission humanity release key", trusted_comment
    )
    out = out or path + ".minisig"
    with io.open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sig)
    return out


def resign_manifest(path, key_id, seed, pub_text, out_dir=None, issued_at=None):
    """Re-sign a schema-1 manifest with a fresh `issued_at` and NOTHING else changed.

    The input's signature must verify against `pub_text` (the launcher's compiled-in key) and the
    secret key must be that key's pair, or the output would be refused by every launcher. Only
    `issued_at` is set; every other field is asserted equal after the round trip.
    -> (manifest path, signature path, old issued_at, new issued_at)."""
    with open(path, "rb") as fh:
        data = fh.read()
    sig_path = path + ".minisig"
    if not os.path.isfile(sig_path):
        raise Refusal("no signature at %s -- refusing to re-sign an unsigned manifest" % sig_path)
    with io.open(sig_path, encoding="utf-8") as fh:
        verify_signature(pub_text, data, fh.read())  # raises Refusal on any mismatch
    pub_id, pub_pk = parse_public_key(pub_text)
    if ed25519_public_key(seed) != pub_pk or key_id != pub_id:
        raise Refusal(
            "the secret key is not the pair of the launcher's PUBLIC_KEY (key id %s) -- the "
            "re-signed manifest would be refused by every launcher" % _key_id_hex(pub_id)
        )
    old = json.loads(data)
    if old.get("schema") != SCHEMA:
        raise Refusal("--resign handles schema %d only, this is %r" % (SCHEMA, old.get("schema")))
    old_issued = old["issued_at"]
    new_issued = issued_at or _now_rfc3339()
    if new_issued < old_issued:
        raise Refusal("new issued_at %s is older than the input's %s" % (new_issued, old_issued))
    new = dict(old)  # insertion order kept: issued_at stays where it is
    new["issued_at"] = new_issued
    new_bytes = manifest_bytes(new)
    trusted = "mission humanity %s, issued %s" % (new.get("version", "?"), new_issued)
    sig = sign_bytes(
        key_id, seed, new_bytes, "signature from the mission humanity release key", trusted
    )
    # What is about to be written must verify and differ from the input only in issued_at.
    verify_signature(pub_text, new_bytes, sig)
    back = json.loads(new_bytes)

    def rest(d):
        return {k: v for k, v in d.items() if k != "issued_at"}

    if rest(back) != rest(old) or list(back) != list(old) or back["issued_at"] != new_issued:
        raise Refusal("the re-serialised manifest differs beyond issued_at")
    out_dir = out_dir or os.path.dirname(os.path.abspath(path))
    os.makedirs(out_dir, exist_ok=True)
    mpath = os.path.join(out_dir, os.path.basename(path))
    spath = mpath + ".minisig"
    with open(mpath, "wb") as fh:
        fh.write(new_bytes)
    with io.open(spath, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sig)
    return mpath, spath, old_issued, new_issued


# --------------------------------------------------------------------------- schema 2 (RL8)
#
# docs/release.md section 7 and tmp/wave_v020/RL8_design.md carry the contract. In one paragraph: the
# launcher and the game are separate signed files per channel (channels/<channel>/launcher.json and
# game.json). `kind` and `channel` are INSIDE the signed body, so a stable file served at the latest
# URL (or a game file at the launcher's) is refused by the client. `latest` is what a tag publishes;
# `stable` is only ever produced by --promote, which re-signs the SAME artifacts (sha256/size/url
# untouched) with a new channel and issued_at.

_RFC3339Z = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


def _ver_key(v):
    """A sort key for `MAJOR.MINOR.PATCH[-pre]`: numeric core, a release above its own prereleases,
    prerelease dot-parts compared numerically when both are numbers (semver; rc7.1 < rc10)."""
    core, _, pre = str(v).partition("-")
    try:
        nums = tuple(int(x) for x in core.split("."))
    except ValueError:
        raise Refusal("%r is not a MAJOR.MINOR.PATCH[-suffix] version" % (v,))
    parts = (
        tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")) if pre else ()
    )
    return (nums, 1 if not pre else 0, parts)


def _https(url, what):
    if not isinstance(url, str) or not url.startswith("https://") or len(url) <= 8:
        raise Refusal("%s %r is not an https:// URL" % (what, url))


def _digest(d, what):
    if not isinstance(d, str) or len(d) != 64 or any(c not in "0123456789abcdef" for c in d):
        raise Refusal("%s is not 64 lower-case hex digits" % what)


def validate_v2(m, kind=None, channel=None):
    """The shape the launcher's accept_v2 expects. -> m. Refuses on the first deviation."""
    if not isinstance(m, dict) or m.get("schema") != SCHEMA2:
        raise Refusal("not a schema-2 manifest (schema %r)" % (m.get("schema"),))
    if m.get("kind") not in KINDS or (kind and m["kind"] != kind):
        raise Refusal("kind is %r, expected %r" % (m.get("kind"), kind or KINDS))
    if m.get("channel") not in CHANNELS or (channel and m["channel"] != channel):
        raise Refusal("channel is %r, expected %r" % (m.get("channel"), channel or CHANNELS))
    _ver_key(m.get("version", ""))
    if not _RFC3339Z.match(str(m.get("issued_at", ""))):
        raise Refusal("issued_at %r is not RFC 3339 UTC (…Z)" % (m.get("issued_at"),))
    if m["kind"] == "launcher":
        _https(m.get("url"), "launcher url")
        _digest(m.get("sha256"), "launcher sha256")
        if not isinstance(m.get("size"), int) or m["size"] <= 0:
            raise Refusal("launcher size must be a positive integer")
    else:
        _ver_key(m.get("min_launcher", ""))
        net = (m.get("game") or {}).get("net")
        if not isinstance(net, dict):
            raise Refusal("a game manifest names the `net` zip (game map = net only)")
        _https(net.get("url"), "game url")
        _digest(net.get("sha256"), "game sha256")
        if not isinstance(net.get("size"), int) or net["size"] <= 0:
            raise Refusal("game size must be a positive integer")
        if "relay" in m:
            relay_field(m["relay"].get("addr"), m["relay"].get("key"))
    return m


def v2_trusted_comment(m):
    """Names kind + channel, so a human reading a .minisig sees what it signs. Not load-bearing:
    the same two facts are in the signed body."""
    return "mission humanity %s %s %s, issued %s" % (
        m["kind"],
        m["channel"],
        m["version"],
        m["issued_at"],
    )


def build_launcher_manifest(
    version, launcher_exe, launcher_url, channel="latest", notes_url="", issued_at=None
):
    if channel not in CHANNELS:
        raise Refusal("--channel must be one of %s" % ", ".join(CHANNELS))
    if not launcher_exe or not os.path.isfile(launcher_exe):
        raise Refusal("no launcher executable at %s" % launcher_exe)
    m = {
        "schema": SCHEMA2,
        "kind": "launcher",
        "channel": channel,
        "version": version,
        "issued_at": issued_at or _now_rfc3339(),
        "url": launcher_url,
        "sha256": sha256(launcher_exe),
        "size": os.path.getsize(launcher_exe),
    }
    if notes_url:
        m["notes_url"] = notes_url
    return validate_v2(m, "launcher", channel)


def build_game_manifest(
    version,
    dist_dir,
    asset_base_url,
    channel="latest",
    min_launcher=None,
    notes_url="",
    issued_at=None,
    relay_addr=None,
    relay_key=None,
):
    """A schema-2 game manifest. The game map is `net` only (decision D1); the other two zips are
    still published on the release, the launcher just does not offer them. The digest is re-hashed
    off disk and checked against SHA256SUMS, as in build_manifest."""
    if channel not in CHANNELS:
        raise Refusal("--channel must be one of %s" % ", ".join(CHANNELS))
    relay = relay_field(relay_addr, relay_key)
    dist_dir = os.path.abspath(dist_dir)
    sums = read_sums(dist_dir)
    name = "%s-%s-net.zip" % (BASE_NAME, version)
    path = os.path.join(dist_dir, name)
    if not os.path.isfile(path):
        raise Refusal("MISSING ASSET: %s is not in %s" % (name, dist_dir))
    digest = sha256(path)
    if name not in sums:
        raise Refusal("%s is on disk but not named in %s" % (name, SUMS_NAME))
    if sums[name] != digest:
        raise Refusal(
            "DIGEST MISMATCH: %s hashes to %s but %s says %s"
            % (name, digest, SUMS_NAME, sums[name])
        )
    m = {
        "schema": SCHEMA2,
        "kind": "game",
        "channel": channel,
        "version": version,
        "issued_at": issued_at or _now_rfc3339(),
        "min_launcher": min_launcher or MIN_LAUNCHER,
        "game": {
            "net": {
                "url": join_url(asset_base_url, name),
                "sha256": digest,
                "size": os.path.getsize(path),
            }
        },
    }
    if relay is not None:
        m["relay"] = relay
    if notes_url:
        m["notes_url"] = notes_url
    return validate_v2(m, "game", channel)


def load_verified(path, pub_text):
    """-> (bytes, parsed). The file's .minisig must sit beside it and verify against `pub_text`."""
    with open(path, "rb") as fh:
        data = fh.read()
    sig_path = path + ".minisig"
    if not os.path.isfile(sig_path):
        raise Refusal("no signature at %s -- refusing to trust an unsigned file" % sig_path)
    with io.open(sig_path, encoding="utf-8") as fh:
        verify_signature(pub_text, data, fh.read())
    return data, json.loads(data)


def check_channel_launcher(path, min_launcher, channel, pub_text):
    """The guard: `path` is the launcher.json SERVED for `channel`. A game manifest requiring a
    launcher newer than that one would be a release no player on the channel can install, so it is
    refused here, at signing time. -> the launcher version found."""
    _data, lm = load_verified(path, pub_text)
    validate_v2(lm, "launcher", channel)
    if _ver_key(lm["version"]) < _ver_key(min_launcher):
        raise Refusal(
            "min_launcher %s is newer than the %s launcher (%s) -- release the launcher first "
            "(tag launcher-v%s) or lower --min-launcher"
            % (min_launcher, channel, lm["version"], min_launcher)
        )
    return lm["version"]


def write_signed(out_dir, name, manifest, key_id, seed, trusted, pub_text=None, say=print):
    """Serialise ONCE, sign those bytes, verify what is about to be written, then write. -> paths."""
    data = manifest_bytes(manifest)
    sig = sign_bytes(key_id, seed, data, "signature from the mission humanity release key", trusted)
    verify_signature(pub_text or format_public_key(key_id, ed25519_public_key(seed)), data, sig)
    os.makedirs(out_dir, exist_ok=True)
    mpath = os.path.join(out_dir, name)
    with open(mpath, "wb") as fh:
        fh.write(data)
    with io.open(mpath + ".minisig", "w", encoding="utf-8", newline="\n") as fh:
        fh.write(sig)
    say("  wrote      %-28s %d B (key %s)" % (name, len(data), _key_id_hex(key_id)))
    return mpath, mpath + ".minisig"


def _check_signing_key(key_id, seed, pub_text):
    pub_id, pub_pk = parse_public_key(pub_text)
    if ed25519_public_key(seed) != pub_pk or key_id != pub_id:
        raise Refusal(
            "the secret key is not the pair of the launcher's PUBLIC_KEY (key id %s) -- the "
            "output would be refused by every launcher" % _key_id_hex(pub_id)
        )


def promote_manifest(path, to, key_id, seed, pub_text, out_dir=None, issued_at=None):
    """latest -> stable: the same artifact, re-signed. The input must verify against the launcher's
    PUBLIC_KEY; only `channel` and `issued_at` change, and that is asserted after the round trip
    (sha256, size, url, version, min_launcher, relay are all carried over untouched).
    -> (manifest path, signature path, old issued_at, new issued_at)."""
    if to != "stable":
        raise Refusal("--promote only promotes to `stable`, got %r" % to)
    _check_signing_key(key_id, seed, pub_text)
    _data, old = load_verified(path, pub_text)
    validate_v2(old)
    if old["channel"] != "latest":
        raise Refusal("only a `latest` file can be promoted; this one is %r" % old["channel"])
    new_issued = issued_at or _now_rfc3339()
    if new_issued < old["issued_at"]:
        raise Refusal(
            "new issued_at %s is older than the input's %s" % (new_issued, old["issued_at"])
        )
    new = dict(old)
    new["channel"] = to
    new["issued_at"] = new_issued
    validate_v2(new, old["kind"], to)

    def rest(d):
        return {k: v for k, v in d.items() if k not in ("channel", "issued_at")}

    back = json.loads(manifest_bytes(new))
    if rest(back) != rest(old) or list(back) != list(old):
        raise Refusal("the promoted file differs from the input beyond channel + issued_at")
    out_dir = out_dir or os.path.dirname(os.path.abspath(path))
    mpath, spath = write_signed(
        out_dir,
        os.path.basename(path),
        new,
        key_id,
        seed,
        v2_trusted_comment(new),
        pub_text,
        say=lambda *_a: None,
    )
    return mpath, spath, old["issued_at"], new_issued


def validate_bridge(m):
    """The schema-1 shape an rc7.1 launcher parses (update.rs, schema 1)."""
    if m.get("schema") != SCHEMA:
        raise Refusal("bridge must be schema %d, got %r" % (SCHEMA, m.get("schema")))
    if not _RFC3339Z.match(str(m.get("issued_at", ""))):
        raise Refusal("bridge issued_at %r is not RFC 3339 UTC" % (m.get("issued_at"),))
    lau = m.get("launcher") or {}
    _ver_key(lau.get("version", ""))
    _https(lau.get("url"), "bridge launcher url")
    _digest(lau.get("sha256"), "bridge launcher sha256")
    game = m.get("game") or {}
    if not game:
        raise Refusal("bridge has no game entries")
    for tag, e in game.items():
        _https(e.get("url"), "bridge game %s url" % tag)
        _digest(e.get("sha256"), "bridge game %s sha256" % tag)
        if not isinstance(e.get("size"), int) or e["size"] <= 0:
            raise Refusal("bridge game %s size must be positive" % tag)
    return m


def bridge_manifest(
    from_path, key_id, seed, pub_text, launcher_json=None, out_dir=None, issued_at=None
):
    """Re-issue the schema-1 bridge manifest (RL10). The base is the currently SERVED bridge (it must
    verify against PUBLIC_KEY): its game entries (the three rc7.1 zips), relay, version and notes
    carry over byte for byte. With `launcher_json` (a verified channels/stable/launcher.json) the
    launcher entry becomes that file's version/url/sha256; without it the launcher entry stays as is
    and only issued_at moves. -> (manifest path, sig path, old issued_at, new issued_at, launcher
    version)."""
    _check_signing_key(key_id, seed, pub_text)
    _data, old = load_verified(from_path, pub_text)
    validate_bridge(old)
    new = dict(old)
    if launcher_json:
        _d, lm = load_verified(launcher_json, pub_text)
        validate_v2(lm, "launcher", "stable")
        if _ver_key(lm["version"]) < _ver_key(old["launcher"]["version"]):
            raise Refusal(
                "stable launcher %s is older than the bridge's %s -- refusing to roll the bridge back"
                % (lm["version"], old["launcher"]["version"])
            )
        new["launcher"] = {"version": lm["version"], "url": lm["url"], "sha256": lm["sha256"]}
    new_issued = issued_at or _now_rfc3339()
    if new_issued < old["issued_at"]:
        raise Refusal(
            "new issued_at %s is older than the input's %s" % (new_issued, old["issued_at"])
        )
    new["issued_at"] = new_issued
    validate_bridge(new)
    for k in ("schema", "version", "game", "relay", "notes_url"):
        if new.get(k) != old.get(k) or (k in old) != (k in new):
            raise Refusal("the bridge differs from its base in %r" % k)
    trusted = "mission humanity bridge %s, issued %s" % (new.get("version", "?"), new_issued)
    out_dir = out_dir or os.path.dirname(os.path.abspath(from_path))
    mpath, spath = write_signed(
        out_dir, MANIFEST_NAME, new, key_id, seed, trusted, pub_text, say=lambda *_a: None
    )
    return mpath, spath, old["issued_at"], new_issued, new["launcher"]["version"]


def age_days(path, pub_text, now=None):
    """Verify `path` against `pub_text` and -> its issued_at age in days (float)."""
    import calendar

    _data, m = load_verified(path, pub_text)
    t = calendar.timegm(time.strptime(m["issued_at"], "%Y-%m-%dT%H:%M:%SZ"))
    return ((now if now is not None else time.time()) - t) / 86400.0


# --------------------------------------------------------------------------- selftest


def selftest():
    """Hermetic: a temp tree, fake zips, no network and no launcher build.

    Four things are asserted, and the first is the one that makes the rest mean anything:
      1. RFC 8032's published Ed25519 test vectors, so the hand-carried curve arithmetic is checked
         against the standard rather than against itself.
      2. A generated key round-trips through the minisign key formats, and a signature round-trips
         through this file's verifier -- including the trusted comment's global signature.
      3. Every tamper is caught: a flipped manifest byte, a flipped trusted comment, a different
         key, and a truncated signature.
      4. The build refusals fire: a missing zip, a SHA256SUMS that disagrees with the bytes on disk,
         and an encrypted secret key.
    """
    fails = []
    checked = [0]

    def expect(name, cond):
        checked[0] += 1
        if not cond:
            fails.append(name)
            print("  FAIL  %s" % name)

    def refuses(name, fn):
        checked[0] += 1
        try:
            fn()
        except Refusal:
            return
        except Exception as exc:  # noqa: BLE001
            fails.append("%s (raised %r, expected Refusal)" % (name, exc))
            print("  FAIL  %s -- raised %r" % (name, exc))
            return
        fails.append("%s (did not refuse)" % name)
        print("  FAIL  %s -- did not refuse" % name)

    # 1. RFC 8032 section 7.1, test vectors 1 and 2.
    for seed_hex, pk_hex, msg_hex, sig_hex in (
        (
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33b"
            "acc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        ),
        (
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e1599"
            "6e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        ),
    ):
        seed, pk, msg, sig = (bytes.fromhex(x) for x in (seed_hex, pk_hex, msg_hex, sig_hex))
        expect("RFC 8032 public key for %s..." % seed_hex[:8], ed25519_public_key(seed) == pk)
        expect("RFC 8032 signature for %s..." % seed_hex[:8], ed25519_sign(seed, msg) == sig)
        expect("RFC 8032 verify for %s..." % seed_hex[:8], ed25519_verify(pk, msg, sig))
        expect(
            "RFC 8032 verify rejects a flipped message for %s..." % seed_hex[:8],
            not ed25519_verify(pk, msg + b"\x00", sig),
        )

    # 2. Key + signature round trip through the minisign formats.
    key_id, seed, pk = generate_key()
    pub_text = format_public_key(key_id, pk)
    sec_text = format_secret_key(key_id, seed, pk)
    back_id, back_seed, back_pk = parse_secret_key(sec_text)
    expect("secret key round-trips", (back_id, back_seed, back_pk) == (key_id, seed, pk))
    expect("public key round-trips", parse_public_key(pub_text) == (key_id, pk))
    payload = b'{"schema": 1}\n'
    sig_text = sign_bytes(key_id, seed, payload, "untrusted", "trusted comment text")
    expect("a fresh signature verifies", verify_signature(pub_text, payload, sig_text))

    # 3. Tampering.
    refuses("a flipped payload byte", lambda: verify_signature(pub_text, payload + b" ", sig_text))
    lines = sig_text.splitlines()
    tampered_tc = "\n".join([lines[0], lines[1], "trusted comment: something else", lines[3]])
    refuses(
        "a rewritten trusted comment",
        lambda: verify_signature(pub_text, payload, tampered_tc + "\n"),
    )
    other_id, other_seed, other_pk = generate_key()
    other_pub = format_public_key(other_id, other_pk)
    refuses(
        "a signature from a different key",
        lambda: verify_signature(other_pub, payload, sig_text),
    )
    # ... and the same key id with a different key, so the refusal is the SIGNATURE and not the id.
    forged = format_public_key(key_id, other_pk)
    refuses(
        "the right key id with the wrong key", lambda: verify_signature(forged, payload, sig_text)
    )
    refuses(
        "a truncated signature",
        lambda: verify_signature(pub_text, payload, "\n".join(lines[:2]) + "\n"),
    )
    expect(
        "the other key's own signature still verifies (the check is not vacuous)",
        verify_signature(other_pub, payload, sign_bytes(other_id, other_seed, payload, "u", "t")),
    )

    # 4. The build refusals, against a fake dist directory.
    with tempfile.TemporaryDirectory() as tmp:
        dist = os.path.join(tmp, "dist")
        os.makedirs(dist)
        exe = os.path.join(tmp, "mh_launcher.exe")
        with open(exe, "wb") as fh:
            fh.write(b"MZ not really")
        names = ["%s-1.2.3-%s.zip" % (BASE_NAME, t) for t in TAGS]
        for i, name in enumerate(names):
            with open(os.path.join(dist, name), "wb") as fh:
                fh.write(b"zip %d" % i)
        sums_path = os.path.join(dist, SUMS_NAME)

        def write_sums(subset=None, corrupt=None):
            with io.open(sums_path, "w", encoding="utf-8", newline="\n") as fh:
                for name in subset if subset is not None else names:
                    d = sha256(os.path.join(dist, name))
                    if corrupt == name:
                        d = "0" * 64
                    fh.write("%s  %s\n" % (d, name))

        write_sums()
        m = build_manifest(
            "1.2.3",
            dist,
            "https://example.invalid/download",
            launcher_exe=exe,
            launcher_version="1.2.3",
            issued_at="2026-01-01T00:00:00Z",
        )
        expect("the manifest names every configuration", sorted(m["game"]) == sorted(TAGS))
        expect("schema is stated", m["schema"] == SCHEMA)
        expect(
            "an asset url is the base plus the file name",
            m["game"]["net"]["url"].endswith("/%s-1.2.3-net.zip" % BASE_NAME),
        )
        expect("sizes are real", all(m["game"][t]["size"] > 0 for t in TAGS))
        data = manifest_bytes(m)
        expect("the manifest is valid JSON", json.loads(data)["version"] == "1.2.3")
        expect("the signed bytes end in a newline", data.endswith(b"\n"))
        mpath, spath = write_manifest(dist, m, key_id, seed, say=lambda *_a: None)
        with open(mpath, "rb") as fh:
            on_disk = fh.read()
        with io.open(spath, encoding="utf-8") as fh:
            on_disk_sig = fh.read()
        expect("what was written is what was signed", on_disk == data)
        expect("the written signature verifies", verify_signature(pub_text, on_disk, on_disk_sig))
        expect("no relay given -> no relay field", "relay" not in m)

        # dist LA6: the relay field, both ways, and its refusals. A documented example address
        # (RFC 5737 TEST-NET-1), never a real one -- lint_machine_paths reads this file too.
        mr = build_manifest(
            "1.2.3",
            dist,
            "https://example.invalid/download",
            launcher_exe=exe,
            launcher_version="1.2.3",
            issued_at="2026-01-01T00:00:00Z",
            relay_addr=" 192.0.2.10:7100 ",
            relay_key="ABCDEF0123456789" * 4,
        )
        expect("relay addr is trimmed", mr["relay"]["addr"] == "192.0.2.10:7100")
        expect(
            "relay key is normalised to lower case", mr["relay"]["key"] == "abcdef0123456789" * 4
        )
        keys = list(mr.keys())
        expect(
            "relay sits between game and notes_url",
            keys.index("game") < keys.index("relay") < keys.index("notes_url"),
        )
        rdata = manifest_bytes(mr)
        rsig = sign_bytes(key_id, seed, rdata, "u", "t")
        expect("a relay manifest signs and verifies", verify_signature(pub_text, rdata, rsig))
        back = json.loads(rdata)
        expect(
            "the relay round-trips through the signed bytes",
            back["relay"] == {"addr": "192.0.2.10:7100", "key": "abcdef0123456789" * 4},
        )
        refuses(
            "a flipped relay byte is caught by the signature",
            lambda: verify_signature(pub_text, rdata.replace(b"7100", b"7101"), rsig),
        )
        expect("the word open is an accepted key", relay_field("h:1", "OPEN")["key"] == "open")
        refuses("a relay address without a key", lambda: relay_field("192.0.2.10:7100", ""))
        refuses("a relay key without an address", lambda: relay_field("", "ab" * 32))
        for bad in ("192.0.2.10", "192.0.2.10:0", "192.0.2.10:70000", "h:1 ; x", "a b:1", ":7100"):
            refuses("relay address %r" % bad, lambda b=bad: check_relay_addr(b))
        for bad in ("", "ab" * 31, "zz" * 32, "ab" * 33):
            refuses("relay key %r" % bad[:8], lambda b=bad: check_relay_key(b))

        os.remove(os.path.join(dist, names[-1]))
        refuses(
            "a dist directory missing one configuration",
            lambda: build_manifest("1.2.3", dist, "https://example.invalid", launcher_exe=exe),
        )
        with open(os.path.join(dist, names[-1]), "wb") as fh:
            fh.write(b"zip last")
        write_sums(corrupt=names[0])
        refuses(
            "a SHA256SUMS that disagrees with the bytes on disk",
            lambda: build_manifest("1.2.3", dist, "https://example.invalid", launcher_exe=exe),
        )
        write_sums()
        refuses(
            "a manifest with no launcher entry",
            lambda: build_manifest("1.2.3", dist, "https://example.invalid"),
        )

    # 5. --sign-file and --resign (dist RL8-B1), against the throwaway key above (never a real one).
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "payload.bin")
        with open(f, "wb") as fh:
            fh.write(b"any bytes at all\x00\x01")
        sp = sign_file(f, key_id, seed, trusted_comment="launcher latest")
        with io.open(sp, encoding="utf-8") as fh:
            sf_text = fh.read()
        with open(f, "rb") as fh:
            fbytes = fh.read()
        expect("sign-file output verifies", verify_signature(pub_text, fbytes, sf_text))
        expect("sign-file keeps the trusted comment", "trusted comment: launcher latest" in sf_text)
        refuses(
            "sign-file output vs a changed file",
            lambda: verify_signature(pub_text, fbytes + b"x", sf_text),
        )

        base = {
            "schema": 1,
            "version": "1.2.3",
            "issued_at": "2026-01-01T00:00:00Z",
            "launcher": {"version": "1.2.3", "url": "https://e.invalid/l.exe", "sha256": "ab" * 32},
            "game": {"net": {"url": "https://e.invalid/n.zip", "sha256": "cd" * 32, "size": 7}},
            "relay": {"addr": "192.0.2.10:7100", "key": "ef" * 32},
            "notes_url": "https://e.invalid/notes",
        }
        src = os.path.join(tmp, "in")
        os.makedirs(src)
        mp = os.path.join(src, MANIFEST_NAME)
        bdata = manifest_bytes(base)
        with open(mp, "wb") as fh:
            fh.write(bdata)
        with io.open(mp + ".minisig", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(sign_bytes(key_id, seed, bdata, "u", "t"))
        outd = os.path.join(tmp, "out")
        nm, ns, old_i, new_i = resign_manifest(
            mp, key_id, seed, pub_text, out_dir=outd, issued_at="2026-01-20T00:00:00Z"
        )
        with open(nm, "rb") as fh:
            nbytes = fh.read()
        with io.open(ns, encoding="utf-8") as fh:
            nsig = fh.read()
        expect("resign output verifies", verify_signature(pub_text, nbytes, nsig))
        got = json.loads(nbytes)
        expect("resign sets issued_at", got["issued_at"] == "2026-01-20T00:00:00Z" != old_i)
        expect(
            "resign changes nothing but issued_at",
            {k: v for k, v in got.items() if k != "issued_at"}
            == {k: v for k, v in base.items() if k != "issued_at"},
        )
        expect("resign keeps field order", list(got) == list(base))
        expect(
            "resign is byte-identical apart from the issued_at line",
            nbytes.replace(b"2026-01-20", b"2026-01-01") == bdata,
        )
        refuses(
            "resign with an older issued_at",
            lambda: resign_manifest(
                mp, key_id, seed, pub_text, out_dir=outd, issued_at="2025-01-01T00:00:00Z"
            ),
        )
        # tampered input: one byte of the manifest changed after signing
        tp = os.path.join(tmp, "tampered")
        os.makedirs(tp)
        tm = os.path.join(tp, MANIFEST_NAME)
        with open(tm, "wb") as fh:
            fh.write(bdata.replace(b"1.2.3", b"1.2.4", 1))
        with io.open(tm + ".minisig", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(sign_bytes(key_id, seed, bdata, "u", "t"))
        refuses(
            "resign of a tampered manifest",
            lambda: resign_manifest(tm, key_id, seed, pub_text, out_dir=outd),
        )
        os.remove(tm + ".minisig")
        refuses(
            "resign of an unsigned manifest",
            lambda: resign_manifest(tm, key_id, seed, pub_text, out_dir=outd),
        )
        refuses(
            "resign with a key that is not the trusted key's pair",
            lambda: resign_manifest(mp, other_id, other_seed, pub_text, out_dir=outd),
        )
        refuses(
            "resign of a manifest not signed by the trusted key",
            lambda: resign_manifest(mp, other_id, other_seed, other_pub, out_dir=outd),
        )
    expect("update.rs PUBLIC_KEY is readable", len(_unb64(launcher_public_key())) == 42)

    # 6. Schema 2 (RL8-B2): build, guard, promote, bridge -- all against the throwaway key.
    selftest_schema2(expect, refuses, key_id, seed, pub_text, other_id, other_seed)

    # An encrypted secret key is refused by name rather than mis-parsed.
    raw = bytearray(_unb64(sec_text.splitlines()[-1]))
    raw[2:4] = b"Sc"
    encrypted = "untrusted comment: minisign encrypted secret key\n%s\n" % _b64(bytes(raw))
    refuses("a password-protected secret key", lambda: parse_secret_key(encrypted))
    # A secret pasted as its base64 line alone (how a repository secret usually arrives) parses;
    # a file that is only the comment line does not.
    bare = sec_text.splitlines()[-1] + "\n"
    expect("a bare base64 line parses", parse_secret_key(bare) == (key_id, seed, pk))
    refuses(
        "a comment-only key file",
        lambda: parse_secret_key("untrusted comment: minisign secret key\n"),
    )

    print("gen_update_manifest selftest: %d check(s), %d failure(s)" % (checked[0], len(fails)))
    return 0 if not fails else 1


def selftest_schema2(expect, refuses, key_id, seed, pub_text, other_id, other_seed):
    quiet = {"say": lambda *_a: None}
    with tempfile.TemporaryDirectory() as tmp:
        dist = os.path.join(tmp, "dist")
        os.makedirs(dist)
        exe = os.path.join(tmp, "mh_launcher.exe")
        with open(exe, "wb") as fh:
            fh.write(b"MZ launcher bytes")
        net = "%s-0.2.0-net.zip" % BASE_NAME
        with open(os.path.join(dist, net), "wb") as fh:
            fh.write(b"net zip bytes")
        with io.open(os.path.join(dist, SUMS_NAME), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("%s  %s\n" % (sha256(os.path.join(dist, net)), net))
        base = "https://example.invalid/releases/download"
        t0 = "2026-01-01T00:00:00Z"
        t1 = "2026-02-01T00:00:00Z"

        # -- build: launcher.json and game.json are independent files
        lm = build_launcher_manifest(
            "0.2.0", exe, base + "/mh_launcher.exe", "latest", "https://e.invalid/n", t0
        )
        gm = build_game_manifest(
            "0.2.0",
            dist,
            base,
            "latest",
            issued_at=t0,
            relay_addr="192.0.2.10:7100",
            relay_key="ab" * 32,
        )
        expect(
            "launcher.json fields",
            lm["kind"] == "launcher" and lm["size"] == 17 and lm["sha256"] == sha256(exe),
        )
        expect(
            "game.json: net only, MIN_LAUNCHER default, relay kept",
            list(gm["game"]) == ["net"] and gm["min_launcher"] == MIN_LAUNCHER and "relay" in gm,
        )
        out_l = os.path.join(tmp, "pages", "channels", "latest")
        lp, _ = write_signed(
            out_l, "launcher.json", lm, key_id, seed, v2_trusted_comment(lm), pub_text, **quiet
        )
        with open(lp, "rb") as fh:
            launcher_before = fh.read()
        gp, gs = write_signed(
            out_l, "game.json", gm, key_id, seed, v2_trusted_comment(gm), pub_text, **quiet
        )
        with open(lp, "rb") as fh:
            expect(
                "building game.json leaves launcher.json untouched", fh.read() == launcher_before
            )
        with io.open(gs, encoding="utf-8") as fh:
            sig_text = fh.read()
        expect(
            "trusted comment names kind and channel",
            "trusted comment: mission humanity game latest 0.2.0" in sig_text,
        )
        refuses(
            "a game manifest missing its net zip",
            lambda: build_game_manifest("0.2.1", dist, base),
        )
        refuses(
            "a non-https asset url",
            lambda: build_game_manifest("0.2.0", dist, "http://example.invalid/d"),
        )
        refuses("an unknown channel", lambda: build_game_manifest("0.2.0", dist, base, "beta"))
        refuses(
            "a zero-size launcher",
            lambda: validate_v2(dict(lm, size=0), "launcher", "latest"),
        )
        refuses("kind mismatch", lambda: validate_v2(gm, "launcher"))
        refuses("channel mismatch", lambda: validate_v2(gm, "game", "stable"))

        # -- the min_launcher guard against the SERVED launcher.json
        expect(
            "guard passes when the served launcher is new enough",
            check_channel_launcher(lp, "0.2.0", "latest", pub_text) == "0.2.0",
        )
        refuses(
            "guard refuses a min_launcher newer than the served launcher",
            lambda: check_channel_launcher(lp, "0.3.0", "latest", pub_text),
        )
        refuses(
            "guard refuses a launcher.json served for another channel",
            lambda: check_channel_launcher(lp, "0.2.0", "stable", pub_text),
        )
        expect(
            "version order: rc.7 < rc.10 < release (semver)",
            _ver_key("0.2.0-rc.7") < _ver_key("0.2.0-rc.10") < _ver_key("0.2.0"),
        )

        # -- promote: only channel + issued_at change
        out_s = os.path.join(tmp, "pages", "channels", "stable")
        pm, ps, old_i, new_i = promote_manifest(
            gp, "stable", key_id, seed, pub_text, out_dir=out_s, issued_at=t1
        )
        with open(pm, "rb") as fh:
            pbytes = fh.read()
        with io.open(ps, encoding="utf-8") as fh:
            psig = fh.read()
        expect("promoted file verifies", verify_signature(pub_text, pbytes, psig))
        pj = json.loads(pbytes)
        expect(
            "promote sets channel=stable, issued_at",
            pj["channel"] == "stable" and pj["issued_at"] == t1,
        )
        expect(
            "promote keeps sha256/size/url/min_launcher/relay/version",
            pj["game"] == gm["game"]
            and pj["min_launcher"] == gm["min_launcher"]
            and pj["relay"] == gm["relay"]
            and pj["version"] == gm["version"],
        )
        expect(
            "promote changes nothing but channel + issued_at",
            {k: v for k, v in pj.items() if k not in ("channel", "issued_at")}
            == {k: v for k, v in gm.items() if k not in ("channel", "issued_at")}
            and list(pj) == list(gm),
        )
        expect("promote trusted comment names stable", "game stable 0.2.0" in psig)
        sp_l = promote_manifest(lp, "stable", key_id, seed, pub_text, out_dir=out_s, issued_at=t1)[
            0
        ]
        with open(sp_l, "rb") as fh:
            sl = json.loads(fh.read())
        expect(
            "promoting the launcher keeps its hash and url",
            sl["sha256"] == lm["sha256"] and sl["url"] == lm["url"] and sl["channel"] == "stable",
        )
        refuses(
            "promote of an already-stable file",
            lambda: promote_manifest(pm, "stable", key_id, seed, pub_text, out_dir=out_s),
        )
        refuses(
            "promote to latest",
            lambda: promote_manifest(gp, "latest", key_id, seed, pub_text, out_dir=out_s),
        )
        refuses(
            "promote with an older issued_at",
            lambda: promote_manifest(
                gp,
                "stable",
                key_id,
                seed,
                pub_text,
                out_dir=out_s,
                issued_at="2025-01-01T00:00:00Z",
            ),
        )
        refuses(
            "promote with a key that is not the trusted key's pair",
            lambda: promote_manifest(gp, "stable", other_id, other_seed, pub_text, out_dir=out_s),
        )
        with open(gp, "rb") as fh:
            gbytes = fh.read()
        tg = os.path.join(tmp, "tampered", "game.json")
        os.makedirs(os.path.dirname(tg))
        with open(tg, "wb") as fh:
            fh.write(gbytes.replace(b"0.2.0", b"0.2.1", 1))
        with io.open(tg + ".minisig", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(sig_text)
        refuses(
            "promote of a tampered file",
            lambda: promote_manifest(tg, "stable", key_id, seed, pub_text, out_dir=out_s),
        )

        # -- the age query
        expect(
            "age-days reads issued_at off a verified file",
            abs(age_days(gp, pub_text, now=1767225600 + 86400 * 14) - 14.0) < 1e-6,
        )

        # -- the schema-1 bridge
        old_bridge = {
            "schema": 1,
            "version": "0.2.0-rc7.1",
            "issued_at": t0,
            "launcher": {"version": "0.2.0-rc7.1", "url": base + "/old.exe", "sha256": "ab" * 32},
            "game": {
                tag: {"url": base + "/%s.zip" % tag, "sha256": "cd" * 32, "size": 9} for tag in TAGS
            },
            "relay": {"addr": "192.0.2.10:7100", "key": "ef" * 32},
            "notes_url": "https://e.invalid/rc7.1",
        }
        bdir = os.path.join(tmp, "bridge_in")
        os.makedirs(bdir)
        bp = os.path.join(bdir, MANIFEST_NAME)
        bb = manifest_bytes(old_bridge)
        with open(bp, "wb") as fh:
            fh.write(bb)
        with io.open(bp + ".minisig", "w", encoding="utf-8", newline="\n") as fh:
            fh.write(sign_bytes(key_id, seed, bb, "u", "t"))
        bout = os.path.join(tmp, "bridge_out")
        # (a) refresh only: no stable launcher yet -> launcher entry unchanged
        m1, s1, _o, n1, lv = bridge_manifest(bp, key_id, seed, pub_text, out_dir=bout, issued_at=t1)
        with open(m1, "rb") as fh:
            b1 = fh.read()
        with io.open(s1, encoding="utf-8") as fh:
            expect("bridge refresh verifies", verify_signature(pub_text, b1, fh.read()))
        j1 = json.loads(b1)
        expect(
            "bridge refresh changes only issued_at",
            j1["issued_at"] == t1
            and {k: v for k, v in j1.items() if k != "issued_at"}
            == {k: v for k, v in old_bridge.items() if k != "issued_at"},
        )
        # (b) with the stable launcher: launcher entry copied, three game entries exactly kept
        j2m, _ = write_signed(
            os.path.join(tmp, "stable_l"),
            "launcher.json",
            dict(lm, channel="stable"),
            key_id,
            seed,
            "t",
            pub_text,
            **quiet,
        )
        m2, _s2, _o2, _n2, lv2 = bridge_manifest(
            bp, key_id, seed, pub_text, launcher_json=j2m, out_dir=bout, issued_at=t1
        )
        with open(m2, "rb") as fh:
            j2 = json.loads(fh.read())
        expect("bridge parses as schema 1", validate_bridge(j2) is j2 and j2["schema"] == 1)
        expect(
            "bridge keeps the three rc7.1 game entries exactly",
            sorted(j2["game"]) == sorted(TAGS) and j2["game"] == old_bridge["game"],
        )
        expect(
            "bridge launcher entry = stable launcher.json version/url/sha256",
            j2["launcher"] == {"version": "0.2.0", "url": lm["url"], "sha256": lm["sha256"]}
            and lv2 == "0.2.0",
        )
        expect(
            "bridge keeps relay and notes",
            j2["relay"] == old_bridge["relay"] and j2["notes_url"] == old_bridge["notes_url"],
        )
        refuses(
            "bridge from a channel=latest launcher.json",
            lambda: bridge_manifest(bp, key_id, seed, pub_text, launcher_json=lp, out_dir=bout),
        )
        refuses(
            "bridge refresh with the wrong key",
            lambda: bridge_manifest(bp, other_id, other_seed, pub_text, out_dir=bout),
        )


def generate_key():
    """-> (key_id, seed, public key). `secrets` is the CSPRNG; the key id is random too, exactly as
    minisign does it -- it is an identifier, not a hash of anything."""
    seed = secrets.token_bytes(32)
    return secrets.token_bytes(8), seed, ed25519_public_key(seed)


# --------------------------------------------------------------------------- cli


def main():
    ap = argparse.ArgumentParser(
        description="build and minisign-sign the launcher's update manifest (dist LA2)"
    )
    ap.add_argument("--selftest", action="store_true", help="run the hermetic checks and exit")
    ap.add_argument("--genkey", action="store_true", help="generate a new signing key pair")
    ap.add_argument("--verify", metavar="MANIFEST", help="verify a manifest against --public-key")
    ap.add_argument("--version", help="the release version, without a leading v")
    ap.add_argument("--dist", default=DEFAULT_DIST, help="the packager's output directory")
    ap.add_argument("--out", help="where manifest.json goes (default: --dist)")
    ap.add_argument("--asset-base-url", help="the URL the three zips are served from")
    ap.add_argument("--launcher-exe", help="the built launcher, for its digest")
    ap.add_argument("--launcher-url", help="where the launcher exe is served from")
    ap.add_argument("--launcher-version", help="the launcher's own version (default: --version)")
    ap.add_argument("--notes-url", default="", help="the release notes page")
    ap.add_argument(
        "--relay-addr",
        default=os.environ.get(RELAY_ADDR_ENV, ""),
        help="dist LA6: the relay host:port the launcher provisions (default: $%s)"
        % RELAY_ADDR_ENV,
    )
    ap.add_argument(
        "--relay-key",
        default=os.environ.get(RELAY_KEY_ENV, ""),
        help="dist LA6: that relay's deployment key, 64 hex (default: $%s)" % RELAY_KEY_ENV,
    )
    ap.add_argument("--issued-at", help="override the timestamp (tests; RFC 3339 UTC)")
    ap.add_argument("--secret-key", help="the minisign secret key file")
    ap.add_argument("--public-key", help="the minisign public key file")
    ap.add_argument("--sign-file", metavar="FILE", help="minisign-sign FILE with --secret-key")
    ap.add_argument("--trusted-comment", help="--sign-file: the signed trusted comment")
    ap.add_argument(
        "--resign",
        metavar="MANIFEST",
        help="re-sign a schema-1 manifest with a fresh issued_at (nothing else changes); the "
        "input signature must verify against the PUBLIC_KEY in update.rs. --out is a directory "
        "(default: the input's own, i.e. in place); --sign-file's --out is the .minisig path",
    )
    ap.add_argument("--kind", choices=KINDS, help="schema 2 (RL8): build a game or launcher file")
    ap.add_argument(
        "--channel", default="latest", help="schema 2: the channel stamped INTO the signed body"
    )
    ap.add_argument(
        "--min-launcher",
        default=None,
        help="schema 2 game: oldest launcher that can run it (default: MIN_LAUNCHER = %s)"
        % MIN_LAUNCHER,
    )
    ap.add_argument(
        "--channel-launcher",
        metavar="LAUNCHER_JSON",
        help="guard: the launcher.json SERVED for the channel (its .minisig beside it); refuse a "
        "game whose min_launcher is newer than it",
    )
    ap.add_argument("--promote", metavar="FILE", help="re-sign a latest game/launcher.json as --to")
    ap.add_argument("--to", help="--promote: the target channel (only `stable`)")
    ap.add_argument("--bridge", action="store_true", help="re-issue the schema-1 bridge manifest")
    ap.add_argument("--from-manifest", metavar="FILE", help="--bridge: the currently served bridge")
    ap.add_argument(
        "--launcher-json",
        metavar="FILE",
        help="--bridge: the verified channels/stable/launcher.json whose launcher entry to copy "
        "(absent: keep the launcher entry, refresh issued_at only)",
    )
    ap.add_argument(
        "--age-days", metavar="FILE", help="verify FILE against update.rs PUBLIC_KEY, print its age"
    )
    args = ap.parse_args()

    try:
        if args.selftest:
            return selftest()

        if args.age_days:
            print("%.2f" % age_days(args.age_days, launcher_public_key()))
            return 0

        if args.promote or args.bridge or args.kind:
            if not args.secret_key:
                raise Refusal("this mode needs --secret-key")
            with io.open(args.secret_key, encoding="utf-8") as fh:
                key_id, seed, _pk = parse_secret_key(fh.read())
            pub_text = launcher_public_key()
            if not args.out:
                raise Refusal("this mode needs --out (a directory)")
            if args.promote:
                if args.channel_launcher:
                    _d, pm = load_verified(args.promote, pub_text)
                    if pm.get("kind") == "game":
                        check_channel_launcher(
                            args.channel_launcher, pm["min_launcher"], args.to or "stable", pub_text
                        )
                mp, sp, old_i, new_i = promote_manifest(
                    args.promote, args.to, key_id, seed, pub_text, args.out, args.issued_at
                )
                print("gen_update_manifest: promoted %s: issued_at %s -> %s" % (mp, old_i, new_i))
                return 0
            if args.bridge:
                if not args.from_manifest:
                    raise Refusal("--bridge needs --from-manifest (the served bridge)")
                mp, sp, old_i, new_i, lv = bridge_manifest(
                    args.from_manifest,
                    key_id,
                    seed,
                    pub_text,
                    args.launcher_json,
                    args.out,
                    args.issued_at,
                )
                print(
                    "gen_update_manifest: bridge %s: issued_at %s -> %s, launcher %s"
                    % (mp, old_i, new_i, lv)
                )
                return 0
            if not args.version:
                raise Refusal("--version is required")
            if args.kind == "launcher":
                m = build_launcher_manifest(
                    args.version,
                    args.launcher_exe,
                    args.launcher_url,
                    args.channel,
                    args.notes_url,
                    args.issued_at,
                )
            else:
                if not args.asset_base_url:
                    raise Refusal(
                        "--asset-base-url is required: the manifest carries absolute URLs"
                    )
                min_l = args.min_launcher or MIN_LAUNCHER
                if args.channel_launcher:
                    got = check_channel_launcher(
                        args.channel_launcher, min_l, args.channel, pub_text
                    )
                    print(
                        "gen_update_manifest: guard ok -- served %s launcher is %s"
                        % (args.channel, got)
                    )
                m = build_game_manifest(
                    args.version,
                    args.dist,
                    args.asset_base_url,
                    args.channel,
                    min_l,
                    args.notes_url,
                    args.issued_at,
                    args.relay_addr,
                    args.relay_key,
                )
            _check_signing_key(key_id, seed, pub_text)
            write_signed(
                args.out, "%s.json" % m["kind"], m, key_id, seed, v2_trusted_comment(m), pub_text
            )
            print(
                "gen_update_manifest: %s.json %s %s, signed by key %s"
                % (m["kind"], m["channel"], m["version"], _key_id_hex(key_id))
            )
            if m["kind"] == "game":
                if "relay" in m:
                    print(
                        "gen_update_manifest: relay %s (key: %d chars, in the signed body)"
                        % (m["relay"]["addr"], len(m["relay"]["key"]))
                    )
                else:
                    print("gen_update_manifest: no relay in this manifest")
            return 0

        if args.genkey:
            if not args.secret_key or not args.public_key:
                raise Refusal("--genkey needs --secret-key and --public-key")
            if os.path.exists(args.secret_key):
                raise Refusal(
                    "%s already exists -- refusing to overwrite a signing key" % args.secret_key
                )
            key_id, seed, pk = generate_key()
            with io.open(args.secret_key, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(format_secret_key(key_id, seed, pk))
            with io.open(args.public_key, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(format_public_key(key_id, pk))
            print("key id %s" % _key_id_hex(key_id))
            print("secret key -> %s   KEEP THIS OUT OF THE REPOSITORY" % args.secret_key)
            print("public key -> %s" % args.public_key)
            print("the line to compile into the launcher (update.rs PUBLIC_KEY):")
            print("    %s" % public_key_line(key_id, pk))
            return 0

        if args.sign_file:
            if not args.secret_key:
                raise Refusal("--sign-file needs --secret-key")
            with io.open(args.secret_key, encoding="utf-8") as fh:
                key_id, seed, _pk = parse_secret_key(fh.read())
            sp = sign_file(args.sign_file, key_id, seed, args.trusted_comment, args.out)
            print(
                "gen_update_manifest: signed %s -> %s (key %s)"
                % (args.sign_file, sp, _key_id_hex(key_id))
            )
            return 0

        if args.resign:
            if not args.secret_key:
                raise Refusal("--resign needs --secret-key")
            with io.open(args.secret_key, encoding="utf-8") as fh:
                key_id, seed, _pk = parse_secret_key(fh.read())
            mp, sp, old_i, new_i = resign_manifest(
                args.resign,
                key_id,
                seed,
                launcher_public_key(),
                out_dir=args.out,
                issued_at=args.issued_at,
            )
            print("gen_update_manifest: re-signed %s: issued_at %s -> %s" % (mp, old_i, new_i))
            print(
                "gen_update_manifest: wrote %s + %s (verified against update.rs PUBLIC_KEY)"
                % (mp, sp)
            )
            return 0

        if args.verify:
            with open(args.verify, "rb") as fh:
                data = fh.read()
            sig_path = args.verify + ".minisig"
            if not os.path.isfile(sig_path):
                sig_path = os.path.join(os.path.dirname(args.verify), SIGNATURE_NAME)
            with io.open(sig_path, encoding="utf-8") as fh:
                sig_text = fh.read()
            if args.public_key:
                with io.open(args.public_key, encoding="utf-8") as fh:
                    pub_text = fh.read()
            else:  # the key players trust, not the one you happen to hold
                pub_text = launcher_public_key()
            verify_signature(pub_text, data, sig_text)
            m = json.loads(data)
            print(
                "OK: %s schema %s kind %s channel %s version %s issued %s"
                % (
                    os.path.basename(args.verify),
                    m.get("schema"),
                    m.get("kind", "-"),
                    m.get("channel", "-"),
                    m.get("version"),
                    m.get("issued_at"),
                )
            )
            return 0

        if not args.version:
            raise Refusal("--version is required (or use --genkey / --verify / --selftest)")
        if not args.asset_base_url:
            raise Refusal("--asset-base-url is required: the manifest carries absolute URLs")
        if not args.secret_key:
            raise Refusal("--secret-key is required: an unsigned manifest is not publishable")
        with io.open(args.secret_key, encoding="utf-8") as fh:
            key_id, seed, _pk = parse_secret_key(fh.read())
        manifest = build_manifest(
            args.version,
            args.dist,
            args.asset_base_url,
            launcher_version=args.launcher_version,
            launcher_exe=args.launcher_exe,
            launcher_url=args.launcher_url,
            notes_url=args.notes_url,
            issued_at=args.issued_at,
            relay_addr=args.relay_addr,
            relay_key=args.relay_key,
        )
        out = args.out or args.dist
        write_manifest(out, manifest, key_id, seed)
        print(
            "gen_update_manifest: %s %s, %d asset(s), signed by key %s"
            % (MANIFEST_NAME, manifest["version"], len(manifest["game"]), _key_id_hex(key_id))
        )
        if "relay" in manifest:
            # The address is printed (a player can read it out of the published file anyway); the
            # key is not, even though it too is public once shipped -- a CI log is a wider audience
            # than a manifest fetch, and GitHub's secret masking should never be the only reason a
            # value is missing from a log.
            print(
                "gen_update_manifest: relay %s (key: %d chars, in the signed body)"
                % (manifest["relay"]["addr"], len(manifest["relay"]["key"]))
            )
        else:
            print(
                "gen_update_manifest: no relay in this manifest (neither %s nor %s was given)"
                % (RELAY_ADDR_ENV, RELAY_KEY_ENV)
            )
        return 0
    except Refusal as exc:
        print("gen_update_manifest: REFUSED -- %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
