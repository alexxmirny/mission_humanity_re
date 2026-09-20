// mh_net_proto -- SESSION_INFO: the session descriptor advertised by a host and listed in a client's
// browser (over LAN or the relay). Portable, no platform dependencies. See S1.
//
// Design (2026-07-12 discussion): the descriptor carries IDENTITY, not LOCATION. There is deliberately
// NO host address -- for the relay the IP is private/meaningless (only the relay keeps it), and for LAN
// the socket is already open; a join routes by lobby-id over the existing channel. The lobby-id = game
// name + a host-generated random tag (so two same-named games don't collide + it is the join/dedup key).
#pragma once
#include <cstdint>
#include <cstddef>
#include "mh_net_proto/uuid7.h" // match_id: the UUIDv7 both records carry (SES0)

namespace mh_net_proto {

constexpr int SESSION_NAME_MAX = 31;   // max game-name chars (excl. NUL)
constexpr int SESSION_MAP_MAX  = 31;   // max map-name chars (excl. NUL)
constexpr int PLAYER_NAME_MAX  = 31;   // max player-name chars (excl. NUL); matches mh.exe's 32-byte name field
constexpr int MAP_HEADER_SIZE  = 0x17c;// mh.exe map_data_1 header size (the browser preview renders from this)

// ---- mp:X2 -- THE MAP'S CONTENT HASH --------------------------------------------------------------
//
// WHY A HASH AND NOT THE NAME. Two installs can hold different bytes under one map name and nothing
// on the wire could tell: a map is a file in `Maps\`, players trade them, and an edited copy keeps
// its filename. The lobby already ships the map's 0x17c HEADER (v2/S9) and that is not an identity
// either -- it is size, biome and start-position count, which two genuinely different maps share
// routinely. So the identity is the SHA-256 of the whole file, and the name is only a label.
//
// EIGHT BYTES, and where the number comes from. A truncated SHA-256 is still a collision-resistant
// identifier at 2^-32 for an ACCIDENTAL collision over any map set a player will ever hold, and the
// threat here is accident, not forgery -- a peer that wanted to feed you a hostile map holds the
// pre-shared key and could simply host one. Eight bytes cost 8 of the ~470-byte advert and 16 hex
// characters of the stored filename; thirty-two would cost 64 characters of a name a human reads in
// a directory listing. The RECEIVER re-hashes the file it stored and compares all eight, so the
// filename is a disambiguator and never the check.
constexpr int         MAP_HASH_BYTES   = 8;
constexpr std::size_t MAP_HASH_HEX_CAP = MAP_HASH_BYTES * 2 + 1; // 16 hex + NUL
// "<stem>.<16 lowercase hex>.<ext>" over a base of at most SESSION_MAP_MAX chars, plus NUL.
constexpr std::size_t MAP_STORED_NAME_CAP = SESSION_MAP_MAX + 1 + (MAP_HASH_BYTES * 2) + 1;

// The SESSION_INFO wire format version (bumped if the layout below changes). Distinct from `protocol`,
// which is the mh.exe GAME protocol version.
// v1: identity + map name string. v2 (S9): + the full 0x17c map_header (real biome/size for the browser
// preview). v3 (SES0): + the 16-byte UUIDv7 match_id, appended after the map header. v4 (mp:F3): + the
// 2-byte input CODEPAGE this host has pinned. v5 (mp:X2): + the 8-byte map CONTENT hash and the map
// file's size. Decode accepts ALL FIVE -- a v1 advert lists fine without
// a preview header, a v2 one without a match_id (nil), a v3 one without a codepage (0).
//
// A pre-SES0 CLIENT, whose decoder accepts only 1 and 2, REFUSES a v3 advert outright and logs
// "malformed SESSION_INFO" rather than mis-parsing it. That is the intended shape of the bump: the
// trailing field would otherwise be silently ignored by a peer that then reports a match under no id.
// The same reasoning applies one step later to v4: a pre-F3 client refuses a v4 advert rather than
// listing a lobby whose text encoding it cannot see.
//
// AND ONE STEP LATER AGAIN FOR v5, which is the strongest case of the three: a pre-X2 client cannot
// see the map's content hash, so it would list a lobby, join it, and load WHATEVER FILE it happens
// to hold under that map's name -- the silent same-name-different-content desync X2 exists to make
// impossible. Refusing the advert is the honest answer.
constexpr std::uint8_t SESSION_INFO_FORMAT = 5;

struct SessionInfo {
    std::uint32_t tag         = 0;   // host-generated random -> disambiguates same-named games (dedup key)
    std::uint16_t host_version = 0;  // host build version (compat gate / display)
    std::uint16_t protocol    = 0;   // mh.exe game protocol version
    std::uint8_t  cur_players = 0;   // players currently in the lobby
    std::uint8_t  max_players = 0;   // lobby capacity
    char name[SESSION_NAME_MAX + 1] = {0};  // game name (host-typed), NUL-terminated
    char map [SESSION_MAP_MAX  + 1] = {0};  // map name, NUL-terminated
    bool has_map_header = false;            // true iff map_header carries the host's real 0x17c map header (v2+)
    unsigned char map_header[MAP_HEADER_SIZE] = {0};  // full map_data_1 header -> the browser map PREVIEW (S9)
    // SES0: the MACHINE identity of this match -- minted by the host at lobby creation, re-minted on
    // host-leave/re-create (beside the tag reset), echoed by every joiner, logged by every peer. All
    // zero = "none yet / a pre-v3 advert" (uuid7_is_nil). `name`+`tag` remain the HUMAN label; this is
    // what correlates two machines' logs, as (match_id, peer_slot, step). Plan decision D8.
    std::uint8_t match_id[UUID7_BYTES] = {0};
    // F3: the INPUT CODEPAGE this host has pinned (mh.dll's `[input] codepage`, i.e.
    // MH_ChatInput_Codepage()). The game's text is 8-bit throughout -- chat is char[81], player and
    // game names are char[32] -- so a byte only denotes a character RELATIVE TO A CODEPAGE, and until
    // F3 that codepage was silently whatever each machine's ANSI ACP happened to be. Advertising it
    // makes the session's encoding explicit, and join_admit() refuses a peer that disagrees rather
    // than seating one whose chat is mojibake on arrival with no error anywhere. 0 = "a pre-v4 advert
    // made no claim", never "codepage zero".
    std::uint16_t codepage = 0;
    // X2: WHAT THE MAP ACTUALLY IS. `map` above is the name a human reads and the name the game
    // opens; these two say which BYTES that name must denote on every peer. All-zero = "this host
    // makes no content claim" (a pre-v5 advert, or a host whose map file could not be read) -- never
    // "a map that hashes to zero"; a joiner that gets no claim falls back to its own local file, the
    // pre-X2 behaviour, rather than refusing to play.
    std::uint8_t  map_hash[MAP_HASH_BYTES] = {0};
    std::uint32_t map_size                 = 0; // the file's length in bytes (0 with no claim)
};

// Worst-case encoded size: 11 scalar bytes + two length-prefixed strings + the fixed 0x17c map header
// (v2) + the 16-byte match_id (v3) + the 2-byte codepage (v4) + the 8-byte map hash and 4-byte map
// size (v5).
constexpr std::size_t SESSION_INFO_MAX_ENCODED = 11 + (1 + SESSION_NAME_MAX) +
                                                 (1 + SESSION_MAP_MAX) + MAP_HEADER_SIZE +
                                                 UUID7_BYTES + 2 + MAP_HASH_BYTES + 4; // 473 + 12 = 485

// Serialize `si` little-endian into `out` (must hold >= SESSION_INFO_MAX_ENCODED bytes). name/map are
// length-prefixed (only the used bytes go on the wire). Returns the number of bytes written.
std::size_t session_info_encode(const SessionInfo& si, std::uint8_t* out) noexcept;

// Deserialize a SESSION_INFO from `in`/`len`. Returns false on a truncated buffer, an unknown format
// version, or an over-long string field. `out` is left well-formed (NUL-terminated) only on success.
bool session_info_decode(const std::uint8_t* in, std::size_t len, SessionInfo& out) noexcept;

// ---- where a SESSION_INFO CAME FROM (mp:R2) ------------------------------------------------------
// A transport module hands mh.dll a received SESSION_INFO through one callback,
// `MH_SessionInfoCb(int sender, const unsigned char*, int)`, whose `sender` is the peer id the record
// arrived from. A record read out of a RELAY DIRECTORY has no such peer: nobody is connected yet --
// that is the whole point of a browser -- and what the listener needs instead is the relay ROOM,
// because dialling that room is what joining a listed lobby MEANS.
//
// WHY THIS IS NOT A 24TH MODULE EXPORT. `MH_NET_MODULE_SYMBOLS` is the mh.dll <-> module ABI and BOTH
// transports must answer all of it; a session directory is a UDP-relay concept, so a new row would
// oblige the TCP module (which must stay byte-for-byte the build it is) to answer a question it
// cannot have. `sender` already carries "where did this come from", and a peer id is 0..7 -- so the
// high half of that int is free, and putting the room there says the new thing in the field that
// already means it. A module that never relays never produces one of these values.
//
// The room is a u32 on the wire and 30 bits here, and since mp:R6 the HOST draws its room UNDER this
// ceiling (udp_room.h masks the draw to SESSION_RELAY_ROOM_MAX) precisely so every minted room is
// routable. A room that did not fit is UNROUTABLE rather than mis-routed --
// session_sender_for_relay_room returns SESSION_SENDER_NONE and the row is dropped.
constexpr int           SESSION_SENDER_RELAY_BASE = 0x40000000;  // sender = BASE | room
constexpr std::uint32_t SESSION_RELAY_ROOM_MAX    = 0x3FFFFFFFu; // the 30 bits the field has
constexpr int           SESSION_SENDER_NONE       = -1;          // "no sender to name"

inline int session_sender_for_relay_room(std::uint32_t room) noexcept {
    if (room > SESSION_RELAY_ROOM_MAX) return SESSION_SENDER_NONE;
    return SESSION_SENDER_RELAY_BASE | (int)room;
}
inline bool session_sender_is_relay(int sender) noexcept {
    return sender >= SESSION_SENDER_RELAY_BASE;
}
inline std::uint32_t session_sender_relay_room(int sender) noexcept {
    return (std::uint32_t)(sender & (int)SESSION_RELAY_ROOM_MAX);
}

// ---- lobby-id (name + tag) ----------------------------------------------------------------------
// Two sessions denote the SAME game (lobby) iff both name AND tag match. Same name + different tag =>
// two distinct games (the tag is what prevents a duplicate-name collision).
bool session_same_lobby(const SessionInfo& a, const SessionInfo& b) noexcept;

// Format the lobby-id as "name#XXXXXXXX" (tag in hex) into `out` (cap bytes). Returns `out`.
const char* lobby_id_str(const SessionInfo& si, char* out, std::size_t cap) noexcept;

// ---- match_id (SES0) -----------------------------------------------------------------------------
// THE EXACT LINE every peer writes to mh_net.log once per match, at lobby create (host) and at the
// first advert carrying a new id (client):
//
//     ; [session] match_id=<32 lowercase hex>
//
// tools/mp_analyze.py parses this literally and pairs the two peers' logs by it, so the prefix, the
// single space, the `match_id=` key and the dash-less 32-hex value are a CONTRACT, not a formatting
// choice. Build it here rather than at each of the three call sites, so there is one spelling.
// `out` needs >= SESSION_LOG_LINE_CAP bytes; the line ends with '\n'. Returns `out`.
constexpr std::size_t SESSION_LOG_LINE_CAP = 64;
const char* session_match_id_log_line(const std::uint8_t match_id[UUID7_BYTES], char* out, std::size_t cap) noexcept;

// ---- JOIN request (client -> host) --------------------------------------------------------------
// "I want to join the lobby identified by <name + tag>, and here is my player name." Carries the
// lobby-id (identity, no address -- the channel implies location, like SessionInfo) PLUS the joining
// player's name (S6: so the host can show the client's real name in its lobby slot instead of "Player2").
// The host matches the lobby-id against its own advertised session; a match admits the sender. See S4/S6.
// v1: lobby-id only. v2 (S6): + player_name. v3 (SES0): + the 16-byte match_id echoed back from the
// advert the client is answering. v4 (mp:F3): + the JOINER's own pinned input codepage. v5 (mp:X2):
// + the map CONTENT HASH this joiner already holds for the advertised map. Decode PARSES
// all five (so the host can say WHICH old version it is refusing); the admit decision below is what
// applies the minimum.
//
// THE v5 FIELD IS THE MAP REQUEST, and it is a field rather than a frame on purpose. "I need the
// map" is a statement about the JOIN -- it is true exactly once, at the moment this peer names the
// lobby it wants to enter, and the host must know it before it can let anyone Start. A separate
// request frame would be a second thing that can be lost, a second thing to retry, and a second
// piece of state to reconcile with the join; carrying the answer in the message that already asks
// to be admitted means a peer that joined has, by construction, already said what it holds.
constexpr std::uint8_t JOIN_REQUEST_FORMAT = 5;

// The oldest JOIN a SES0 host admits. A v1/v2 client carries no match_id, so admitting it would seat a
// peer whose logs can never be correlated with the host's -- the whole point of SES0. It is refused by
// join_admit() with a NAMED reason rather than dropped silently, which is the negative case the
// tracker item asks for. Raise this in step with JOIN_REQUEST_FORMAT whenever a later field becomes
// load-bearing for admission; do NOT raise it merely because the format grew.
//
// RAISED TO 4 BY mp:F3, and that rule is the reason rather than an exception to it: the codepage IS
// load-bearing for admission. A v3 client carries none, so the host cannot tell whether that peer's
// chat bytes will denote the same characters -- and "assume it agrees" is precisely the silent
// corruption the field was added to prevent. So a v3 peer is refused as an old protocol, by name.
//
// RAISED TO 5 BY mp:X2, on the identical reasoning one field further along. A v4 client never says
// which map bytes it holds, so the host cannot tell whether it needs the file -- and the host's
// Start gate is defined as "no admitted joiner's download is incomplete", a predicate that is
// VACUOUSLY TRUE for a peer that never reported. Admitting one would mean launching a match in which
// exactly one player may be simulating a different map, which is the failure this item exists to
// remove. So a v4 peer is refused as an old protocol, by name, exactly as a v3 one is.
constexpr std::uint8_t JOIN_REQUEST_MIN_FORMAT = 5;

struct JoinRequest {
    std::uint32_t tag = 0;                          // lobby-id tag (must equal the host's SessionInfo.tag)
    char name[SESSION_NAME_MAX + 1] = {0};          // lobby-id game name, NUL-terminated
    char player_name[PLAYER_NAME_MAX + 1] = {0};    // the joining player's name (S6), NUL-terminated
    std::uint8_t match_id[UUID7_BYTES] = {0};       // SES0: the advert's match_id, echoed back (nil for v1/v2)
    std::uint16_t codepage = 0;                     // F3: the JOINER's OWN pinned codepage (0 for v1..v3)
    // X2: the content hash this joiner ALREADY HOLDS for the advertised map, all-zero for "I hold
    // nothing that matches". It is the joiner's own measurement of its own file -- never an echo of
    // the advert, for the reason `join_request_for`'s two-argument form exists: a field echoed back
    // makes every disagreement look like agreement, which is worse than not having the field.
    std::uint8_t  map_hash[MAP_HASH_BYTES] = {0};
    std::uint8_t format = JOIN_REQUEST_FORMAT;      // the format byte DECODE actually read (not sent; see below)
};

// Worst-case encoded size: format byte + tag + length-prefixed name + length-prefixed player_name +
// the 16-byte match_id (v3) + the codepage (v4) + the 8-byte map hash (v5). `format` is NOT a wire
// field of its own -- encode always writes JOIN_REQUEST_FORMAT, and decode reports what it read
// there so the admit policy can act on it.
constexpr std::size_t JOIN_REQUEST_MAX_ENCODED = 1 + 4 + (1 + SESSION_NAME_MAX) +
                                                 (1 + PLAYER_NAME_MAX) + UUID7_BYTES + 2 +
                                                 MAP_HASH_BYTES; // 87 + 8 = 95

// Serialize `jr` little-endian into `out` (must hold >= JOIN_REQUEST_MAX_ENCODED bytes). Returns bytes written.
std::size_t join_request_encode(const JoinRequest& jr, std::uint8_t* out) noexcept;

// Deserialize a JOIN request from `in`/`len`. Returns false on truncation, an unknown format version,
// or an over-long name. `out` is left well-formed (NUL-terminated) only on success.
bool join_request_decode(const std::uint8_t* in, std::size_t len, JoinRequest& out) noexcept;

// Build a JOIN request naming the same lobby (name + tag) as `si`, echoing its match_id.
//
// `my_codepage` is the JOINER'S OWN pin, not the advert's. Echoing the host's value back would make
// every mismatch look like agreement, which is the one way this field could be worse than not having
// it; the one-argument form therefore means "this peer declares no pin" (0) rather than "whatever the
// host said". Callers that have a pin pass it.
JoinRequest join_request_for(const SessionInfo& si) noexcept;
JoinRequest join_request_for(const SessionInfo& si, std::uint16_t my_codepage) noexcept;
// X2: ...and the three-argument form that also states which map bytes this joiner holds. `my_map_hash`
// may be null for "I hold nothing", which encodes as the all-zero claim.
JoinRequest join_request_for(const SessionInfo& si, std::uint16_t my_codepage,
                             const std::uint8_t* my_map_hash) noexcept;

// True iff `jr` names the same lobby (name AND tag) as host session `si` -- the host's LOBBY test.
// Deliberately still name+tag only: the tag is already re-minted on every lobby re-create, so adding
// match_id here would refuse nothing the tag does not already refuse, while giving the admit path a
// second way to fail during a normal re-join. Version policy lives in join_admit() instead.
bool join_matches_session(const JoinRequest& jr, const SessionInfo& si) noexcept;

// ---- the host's ADMIT decision (SES0) -------------------------------------------------------------
// One function, so the seam that logs it and the selftest that proves it are looking at the same
// code. The seam (mh/seams/net_discovery.cpp, on_join_recv) does nothing but call this and print
// join_admit_reason(); every refusal branch therefore has an offline arm, which matters because
// three of the four cannot be produced on the rig at all (there is no old client to run).
enum class JoinAdmit : std::uint8_t {
    Admit = 0,            // this JOIN names our lobby and is new enough -- open the slot/map gate
    RefusedMalformed,     // truncated, over-long field, or a format byte of 0
    RefusedOldProtocol,   // format < JOIN_REQUEST_MIN_FORMAT: a pre-match_id client
    RefusedNewerProtocol, // format > JOIN_REQUEST_FORMAT: a client newer than this host
    RefusedWrongLobby,    // parsed fine, but names a different name#tag
    RefusedCodepage,      // F3: parsed fine, our lobby, but the joiner pins a DIFFERENT input codepage
};

// A short, stable, greppable reason string for the log line. Never null.
const char* join_admit_reason(JoinAdmit a) noexcept;

// Decode `in`/`len` and decide. `out` is filled whenever the bytes parsed (i.e. for every verdict
// except RefusedMalformed), so the caller can name the rejected peer's lobby-id in its log line.
JoinAdmit join_admit(const std::uint8_t* in, std::size_t len, const SessionInfo& mine, JoinRequest& out) noexcept;

// ---- mp:F3c -- the ANNOUNCE payload, and the one kind of it that is addressed to a single peer ----
//
// FLAG_ANNOUNCE (U16) has always been host -> everybody text: `[0]=kind, [1]=the player id the line
// is about, [2..]=NUL-terminated ANSI`. Two kinds existed and were never named -- 0 "<name> left",
// 1 "<name> joined" -- and a receiver whose own id is byte [1] skips the line, because the retail
// path already announces a joiner to itself. F3c adds the third kind and NAMES all three.
//
// WHY A REFUSAL RIDES THIS FRAME AND NOT A NEW ONE. A refused JOIN used to be a host log line and
// nothing else: the joiner's retail lobby seats it locally before any reply exists, so a peer whose
// JOIN was refused sat in a lobby that showed it seated, with nothing on its screen or in its log
// saying otherwise (measured on the first real internet lobby, 2026-09-19 -- codepage 1252 vs 1251,
// three refusals, the joiner never told). The answer has to reach the joiner over a channel it has,
// and the only host -> client control frames are SESSION_INFO, START and ANNOUNCE; of those ANNOUNCE
// is the one that already carries "a line of text about player N". A new flag would be a new row in
// `MH_NET_MODULE_SYMBOLS`, which both transports must answer, for a message that is by construction
// a line of text about a player. It is BROADCAST like every announce (the transports have no
// unicast); byte [1] says who it is for, and the peers it is not for log it and do nothing.
//
// TEXT, NOT A CODE, on the wire: the joiner shows the reason to a human, and the host is the side
// that knows both codepages, the format range, whatever the refusal was ABOUT. A code would make
// every future refusal a two-sided change. The text is ASCII by construction (the host composes it
// from numbers and English words), so the joiner's own codepage cannot garble it.
//
// AND IT IS SHORT BY CONTRACT: at most JOIN_REFUSAL_TEXT_MAX characters. The joiner paints it on the
// browser's status-line widget behind a "Refused: " prefix, and that widget draws ONE unwrapped line
// ~32 characters wide (measured against U23's "Connection to the host was lost.", which fills it
// exactly). The host's own log carries the long form; the wire carries what fits on the screen.
constexpr std::uint8_t ANNOUNCE_LEFT    = 0; // "<name> left the game"   (U16)
constexpr std::uint8_t ANNOUNCE_JOINED  = 1; // "<name> joined the game" (U16)
constexpr std::uint8_t ANNOUNCE_REFUSED = 2; // F3c: "your JOIN was refused: <reason>", for player [1] only
constexpr std::size_t  ANNOUNCE_TEXT_CAP    = 96;                       // reason text incl. NUL
constexpr std::size_t  ANNOUNCE_MAX_ENCODED = 2 + ANNOUNCE_TEXT_CAP;    // kind + player id + text

// Encode a REFUSED announce for `target_player_id` into `out` (>= ANNOUNCE_MAX_ENCODED). The reason
// is truncated to ANNOUNCE_TEXT_CAP-1 characters and always NUL-terminated. Returns bytes written.
std::size_t announce_refused_encode(std::uint8_t target_player_id, const char* reason, std::uint8_t* out) noexcept;

// Decode one. False unless `in` is a well-formed ANNOUNCE of kind REFUSED (kind byte, id byte, at
// least one text byte, NUL within `len`). `reason` receives the text (cap ANNOUNCE_TEXT_CAP).
bool announce_refused_decode(const std::uint8_t* in, std::size_t len, std::uint8_t* target_player_id,
                             char* reason, std::size_t cap) noexcept;

// The refusal text the host sends, composed from the admit verdict and the two sides' facts, at
// most JOIN_REFUSAL_TEXT_MAX characters (see above): "codepage 1252/1251" (host's first, then the
// joiner's), "protocol 3 < 5", "not our lobby". `out` needs ANNOUNCE_TEXT_CAP bytes. Returns `out`.
constexpr std::size_t JOIN_REFUSAL_TEXT_MAX = 20; // 32 on screen minus the 9-char "Refused: " prefix, 3 spare
const char* join_refusal_text(JoinAdmit a, const SessionInfo& mine, const JoinRequest& theirs, char* out,
                              std::size_t cap) noexcept;

// ---- mp:X2 -- the map-identity helpers ------------------------------------------------------------
//
// PORTABLE ON PURPOSE, like everything else in this library: the DLL seam that hashes a real file,
// the offline suite that proves the naming, and any future tool that has to recognise a downloaded
// map in a directory listing all build the same string from the same bytes. A second spelling of
// "<stem>.<hex>.<ext>" anywhere would be a second answer to "is this file the one the host meant".

// Truncate a full SHA-256 to the wire's 8-byte map hash. Trivial, and named rather than open-coded
// so the truncation happens once and every caller is obviously taking the SAME eight bytes.
void map_hash_from_sha256(const std::uint8_t sha[32], std::uint8_t out[MAP_HASH_BYTES]) noexcept;

// True iff `h` is the all-zero "no claim" value. Never call a real map's hash "none" on any other test.
bool map_hash_is_none(const std::uint8_t h[MAP_HASH_BYTES]) noexcept;
bool map_hash_equal(const std::uint8_t a[MAP_HASH_BYTES], const std::uint8_t b[MAP_HASH_BYTES]) noexcept;

// 16 lowercase hex + NUL into `out` (>= MAP_HASH_HEX_CAP). Returns `out`.
const char* map_hash_hex(const std::uint8_t h[MAP_HASH_BYTES], char* out, std::size_t cap) noexcept;

// THE STORED NAME. "Cold War.mpm" + hash -> "Cold War.3fa2b1c94d8e7a06.mpm": the hash is inserted
// before the LAST dot, so the extension -- which is what the game's own loader uses to decide
// whether the file lives loose in `Maps\` or in the `Dane\` resource path -- is unchanged, and the
// stem still sorts next to the original in a directory listing.
//
// A base with no dot gets the hash appended ("MYMAP" -> "MYMAP.3fa2..."), which is well-defined
// rather than refused because the loader's own name field has no extension requirement either.
// Returns the length written, or 0 if `cap` is too small or `base` is empty -- and 0 means DO NOT
// WRITE A FILE, not "use the base name", because falling back to the base name is precisely the
// overwrite this scheme exists to prevent.
std::size_t map_stored_name(const char* base, const std::uint8_t hash[MAP_HASH_BYTES], char* out,
                            std::size_t cap) noexcept;

// True iff `name` is a stored name produced by map_stored_name from `base` (any hash). Used by the
// resolver to enumerate candidates without re-deriving the layout, and by the suite to prove a
// same-name-different-content local file is never a candidate.
bool map_name_is_stored_form(const char* name, const char* base) noexcept;

} // namespace mh_net_proto
