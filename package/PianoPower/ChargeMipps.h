/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* MiPPS: Xiaomi's charger authentication over PD unstructured VDMs, for the charge pump's 6 A source budget (docs/drivers/
 * charging-policy.md; private/host-checks/20261123-charging/STACK.md). Behaviour, sequence and numbers as the Ubuntu port's kernel
 * charger shows them (read for behaviour only; no code of it here): the ADSP charger firmware carries the VDMs, the host only asks and
 * computes the digest.
 *
 *   - Eligible: a PD source (real type PD, PD verified or PD PPS) with an object above 5 V, MiPPS on (piano-power-mipps), the key file
 *     loaded, no voltage step and no pump yet, not tried on this attachment.
 *   - 0x21002 (VERIFY_PROCESS) = 1, then VDM command n through property 0x21003 + n - 1; after each write the engine waits 10 ms and
 *     polls VBUS (0x20002, >= 4 V or the source is gone) and the UVDM state (0x21012) every 10 ms until it reads n, within the
 *     command's deadline (1: 500 ms, 2: 500, 3: 1000, 4: 1500, 5: 2500, 6: 1000 (500 when its flag is 1), 7: 1000); one resend.
 *   - Command 1, the version at 0x21003 (0 or all ones: no MiPPS adapter), 0x21010 (PD_VERIFIED) = 0, commands 2 and 3, command 4
 *     with one key's 16-byte seed, command 5 with a fresh 16-byte random challenge, the answer at 0x21007 (16 bytes), the adapter ID
 *     at 0x2100e. HMAC-SHA256(key, challenge || adapter ID big endian): its first 16 bytes must be the answer. An ID of 0 or a stale
 *     one: the IDs 0..65535 are tried against this answer only.
 *   - Then command 7 and command 6, both with flag 1 (command 8 stalls this firmware: never sent), VBUS still online, 0x21010 = 1.
 *   - Any failure after the version: 0x21010 = 0 and command 7 with flag 0. In EVERY case 0x21002 = 0 last; until that write is
 *     acknowledged no voltage may be asked (piano_mipps_cleanup_pending).
 *   - Verified: the pump's source budget is 6000 mA; else (no key, not eligible, failed) 5000 mA, standard PD/PPS.
 *
 * The key material (seeds and keys) is NEVER in this repository: the private file EFI/UniXNU/firmware/PianoPower/mipps-keys.bin
 * (format below) is pinned at build time from private/charging/ (tooling/k27/build-native-piano-power.sh), read at boot through
 * <PianoFirmware.h>, checked against its private pin, and kept in kernel memory only. No key file: MiPPS is off, logged.
 *
 * The engine is pure C, no I/O and no clock of its own: the caller sends each request piano_mipps_next gives, hands back its result
 * with piano_mipps_reply, and calls piano_mipps_next again at or after the time it asks for. Host tests: tests/mipps_host.cpp. */
#ifndef PIANO_CHARGE_MIPPS_H
#define PIANO_CHARGE_MIPPS_H
#include <stddef.h>
#include <stdint.h>
#include "ChargePump.h"   /* struct piano_cp_in: the session's MiPPS fields */
#ifdef __cplusplus
extern "C" {
#endif

/* The firmware properties (the MCA owner 0x800a). */
#define PIANO_MIPPS_PROP_VBUS 0x20002u            /* uV, read */
#define PIANO_MIPPS_PROP_VERIFY_PROCESS 0x21002u  /* 1 while an authentication runs, 0 after: write */
#define PIANO_MIPPS_PROP_VDM_BASE 0x21003u        /* command n at 0x21003 + n - 1 */
#define PIANO_MIPPS_PROP_VERSION 0x21003u         /* read after command 1 */
#define PIANO_MIPPS_PROP_AUTH 0x21007u            /* command 5's challenge (write) and the adapter's answer (read), 16 bytes */
#define PIANO_MIPPS_PROP_ADAPTER_ID 0x2100eu      /* read */
#define PIANO_MIPPS_PROP_PD_VERIFIED 0x21010u     /* 0 / 1: write */
#define PIANO_MIPPS_PROP_UVDM_STATE 0x21012u      /* the last command the adapter answered: read */
#define PIANO_MIPPS_VDM_BYTES 16u
#define PIANO_MIPPS_VBUS_ONLINE_UV 4000000u
#define PIANO_MIPPS_POLL_MS 10u
#define PIANO_MIPPS_ID_SEARCH 65536u
#define PIANO_MIPPS_ID_CHUNK 256u                 /* IDs tried per piano_mipps_next call (the caller's thread yields between) */
#define PIANO_MIPPS_BUDGET_VERIFIED_MA 6000u
#define PIANO_MIPPS_BUDGET_STANDARD_MA 5000u

/* The private key file: "PMIPKEY1", u32 LE count (1..PIANO_MIPPS_KEYS_MAX), u32 LE 48, count x {seed[16], key[32]}, then the
 * SHA-256 of everything before it. Parsed by piano_mipps_keys_parse; the bytes are wiped by piano_mipps_keys_wipe. */
#define PIANO_MIPPS_KEYS_MAX 16u
#define PIANO_MIPPS_KEY_FILE "PianoPower/mipps-keys.bin"
typedef struct {
    uint8_t seed[16];
    uint8_t key[32];
} PianoMippsKey;
typedef struct {
    uint32_t count;   /* 0: no key, MiPPS off */
    PianoMippsKey k[PIANO_MIPPS_KEYS_MAX];
} PianoMippsKeys;
/* 0 and *out filled, or a negative PIANO_MIPPS_E_* (out->count 0). */
int piano_mipps_keys_parse(const uint8_t *data, size_t bytes, PianoMippsKeys *out);
void piano_mipps_keys_wipe(PianoMippsKeys *keys);

/* One request to the firmware. */
enum {
    PIANO_MIPPS_REQ_NONE = 0,    /* nothing now: call again at wait_until_ms (or when the result is final) */
    PIANO_MIPPS_REQ_WRITE = 1,   /* write len bytes of data (4: one u32 LE, 16: four u32 LE words) to prop */
    PIANO_MIPPS_REQ_READ = 2,    /* read len bytes of prop (4 or 16) */
};
typedef struct {
    uint32_t kind;
    uint32_t prop;
    uint32_t len;
    uint8_t data[PIANO_MIPPS_VDM_BYTES];
    uint64_t wait_until_ms;      /* REQ_NONE: the time to call piano_mipps_next again (0: done) */
} PianoMippsRequest;

/* Errors (negative), also the result's error. */
enum {
    PIANO_MIPPS_OK = 0,
    PIANO_MIPPS_E_NOKEY = -1,     /* no key file */
    PIANO_MIPPS_E_FORMAT = -2,    /* key file malformed */
    PIANO_MIPPS_E_IO = -3,        /* a firmware read or write failed */
    PIANO_MIPPS_E_GONE = -4,      /* VBUS below 4 V, a detach or a new firmware service: the source is gone */
    PIANO_MIPPS_E_TIMEOUT = -5,   /* a command not answered within its deadline, twice */
    PIANO_MIPPS_E_NOADAPTER = -6, /* version 0 or all ones: not a MiPPS adapter */
    PIANO_MIPPS_E_REJECTED = -7,  /* the answer matches no adapter ID */
    PIANO_MIPPS_E_BUSY = -8,      /* started while running */
};

/* Where the engine is (for the log and the registry). */
enum {
    PIANO_MIPPS_IDLE = 0, PIANO_MIPPS_RUNNING = 1, PIANO_MIPPS_CLEANUP = 2, PIANO_MIPPS_DONE = 3,
};

typedef struct {
    /* the result, valid when state == PIANO_MIPPS_DONE */
    uint32_t state;
    int32_t error;               /* PIANO_MIPPS_OK when verified */
    uint32_t verified;           /* 1: the adapter answered for one of our keys */
    uint32_t adapter_id;         /* the matched ID (0 unless verified) */
    uint32_t discovered;         /* the adapter answered command 1 with a version */
    uint32_t version;
    uint32_t last_cmd;           /* the last command sent */
    uint32_t cleared;            /* 1: 0x21002 = 0 acknowledged */
    uint32_t sent8;              /* always 0: command 8 is never sent (a test checks it) */
    /* the engine's own state */
    uint32_t step, sub, attempt, cmd, flag, key_index;
    uint32_t id_next, awaiting;   /* awaiting: a request was given and its reply is owed */
    uint64_t deadline_ms, wait_until_ms;
    int32_t pending_error;
    uint8_t challenge[16], response[16];
    const PianoMippsKeys *keys;
} PianoMipps;

/* Eligibility (STACK.md MiPPS row): real_type 10/11/12 (PD, PD PPS, PD verified as the firmware's 0x2000c names them), any PDO
 * above 5000 mV, no voltage step and no pump yet, not tried on this attachment, a key loaded. */
int piano_mipps_eligible(int enabled, const PianoMippsKeys *keys, int real_type, const uint32_t *pdo_max_mv, unsigned pdos,
                         int hv_off, int cp_off, int attempted_this_attach);

/* Starts one authentication: key_index (the caller's random choice below keys->count) and challenge (16 fresh random bytes). */
int piano_mipps_start(PianoMipps *m, const PianoMippsKeys *keys, uint32_t key_index, const uint8_t challenge[16], uint64_t now_ms);
/* The next request (REQ_NONE with wait_until_ms, or 0 when DONE). attach_current: same attachment and firmware service as at start;
 * service_current: same firmware service (a cleanup is written only then). */
void piano_mipps_next(PianoMipps *m, uint64_t now_ms, int attach_current, int service_current, PianoMippsRequest *req);
/* The result of the request last given: status 0 = done (data/len for a READ), else the transport failed. */
void piano_mipps_reply(PianoMipps *m, uint64_t now_ms, int status, const uint8_t *data, uint32_t len);
/* Whether 0x21002 = 0 is still owed: no voltage request until it is acknowledged. */
int piano_mipps_cleanup_pending(const PianoMipps *m);
/* The pump's source budget this result allows (PIANO_MIPPS_BUDGET_*). */
uint32_t piano_mipps_budget_ma(const PianoMipps *m);
const char *piano_mipps_error_name(int error);
/* The session's MiPPS fields of struct piano_cp_in (ChargePump.h): will_run 0 (not eligible: no key, MiPPS off, no PD source with an
 * object above 5 V) gives done 1, not verified, 5000 mA; else done once the engine is DONE, verified, and its budget. */
void piano_mipps_fill_cp_in(const PianoMipps *m, int will_run, struct piano_cp_in *in);

/* HMAC-SHA256 (RFC 2104 / FIPS 180-4), exposed for the host tests (RFC 4231 vectors). */
void piano_mipps_sha256(const uint8_t *data, size_t bytes, uint8_t digest[32]);
void piano_mipps_hmac_sha256(const uint8_t *key, size_t key_bytes, const uint8_t *msg, size_t msg_bytes, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
#endif
