#pragma once
//
// mh_key.txt -- the per-session pre-shared key that authenticates + encrypts the MP transport
// (mh_common/net_key.cpp; crypto in mh_net_proto/net_crypto.h).
//
// DISTRIBUTION MODEL. The host's key is the session's password. On first run the DLL generates one
// and writes it here; the host sends that one line to the people it is playing with (Discord/chat),
// they drop it in their own game folder, and the handshake succeeds. Nobody else can join, and
// nobody on the path can read the traffic. There is deliberately no key exchange over the wire: an
// unauthenticated exchange would be exactly the hole this closes.
//
// This is a FILE and not an ini key on purpose -- the ini is optional in the ship build, and a
// secret that must be copied between machines is easier to explain, send and replace as its own
// one-line file than as a line inside a config nobody else has.
//
#ifdef __cplusplus
extern "C" {
#endif

#define MH_KEY_LEN     32
#define MH_KEY_HEX_LEN 64

/* Key-file states. The transport FAILS CLOSED on anything it cannot understand: a corrupt key file
 * means "refuse to run", never "quietly run without protection". */
enum {
    MH_KEY_SECURE  = 1,  /* a valid 64-hex key was loaded (generating one counts)            */
    MH_KEY_OPEN    = 0,  /* the file says "open": no auth, no encryption -- LAN/testing only */
    MH_KEY_INVALID = -1, /* present but unparseable, or no secure randomness available       */
};

/* Load (or, when the file is missing, GENERATE and persist) the key for `dir` (an exe directory with
 * a trailing separator). On MH_KEY_SECURE, `out_key` holds the 32 raw bytes and `out_hex` (>= 65
 * chars) the text form for logging/sharing. `out_generated` (optional) is set to 1 when this call
 * created the file, so the caller can log the first-run instructions.
 *
 * Writing the file "open" (that literal word) is how a host opts out of authentication entirely --
 * useful for a LAN game between fresh installs, and the only way to interoperate with a pre-ship
 * DLL. That state is logged as a warning every run, because on a published port it means anyone can
 * join and inject into the game's parser. */
int MH_Key_Load(const char *dir, unsigned char *out_key, char *out_hex, int *out_generated);

/* Fill `buf` with `len` cryptographically random bytes; 0 on failure. Used for the per-connection
 * handshake nonces, which is why it is exported rather than kept private to the key file: a
 * predictable nonce would let a captured handshake be replayed. Callers must FAIL on 0, never fall
 * back to a clock-seeded value. */
int MH_Key_Random(unsigned char *buf, unsigned len);

#ifdef __cplusplus
}
#endif
