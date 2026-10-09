/* autopatch60-oneshot.c -- RDR2 (CUSA03041) v1.00 variant.
 * Waits for the RDR2 eboot (max. 10 min), verifies, writes, re-verifies,
 * notifies and EXITS. No residency.
 *
 * v1.00 changes vs the 1.32 build:
 *  - Target is eboot+0x04a8ee1f (RVA 0x468EE1F) instead of 0x05853029.
 *  - The 1.32 64-byte context is gone (it does not match 1.00). Instead:
 *      * relaxed check: target must be `cmove r32,r32` (0f 44 /r, mod==3);
 *        the matching `xor r,r; nop` patch is derived from the register used.
 *      * the 64-byte window around the target is always logged in hex to
 *        LOG_PATH so you can paste it back and enable CTX_STRICT below.
 *  - Set CTX_STRICT to 1 and fill CTX100[] to restore the original strict
 *    64-byte verification once you have the real 1.00 bytes.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PACKET_MAGIC  0xFFAABBCCu
#define WIRE_SUCCESS  0x80000000u

#define CMD_PROC_READ      0xBDAA0002u
#define CMD_PROC_WRITE     0xBDAA0003u
#define CMD_PROC_MAPS_X    0xBDAA0004u
#define CMD_CONSOLE_NOTIFY 0xBDDD0004u
#define CMD_FG_APP         0xBDDD0006u

#define TARGET_RVA 0x468EE1Fu          /* 0x04a8ee1f - 0x400000 (v1.00; confirmed from log: cmove esi,eax sits 2 bytes after the reddit address) */
#define PROT_EXEC 0x4u
#define IMG_BASE_DEFAULT 0x400000u     /* base seen 100% of the time; fast-path only */

#define CTX_OFF 32                     /* target sits at +32 in the 64-byte window */

/* Strict mode: paste the 64 bytes logged as "CTX64 ..." here, set CTX_STRICT 1. */
#define CTX_STRICT 0
#if CTX_STRICT
static const uint8_t CTX100[64] = {0};
#endif

/* USB telemetry (read back later over FTP). Nothing sensitive. */
#define LOG_PATH "/mnt/usb0/autopatch60.log"
static unsigned log_n = 0;

static void tlog(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    fprintf(f, "%u %s\n", log_n++, msg);
    fclose(f);
}

static void log_hex(const char *tag, const uint8_t *p, size_t n) {
    char lb[300];
    size_t i, o;
    o = (size_t)snprintf(lb, sizeof(lb), "%s", tag);
    for (i = 0; i < n && o + 4 < sizeof(lb); i++)
        o += (size_t)snprintf(lb + o, sizeof(lb) - o, " %02x", p[i]);
    tlog(lb);
}

/* cmove r32,r32 = 0f 44 /r with mod==3  ->  xor r32,r32 ; nop = 31 /r 90
 * (31 c0|reg<<3|reg). Returns 0 and fills neu[3] if cur[] is patchable. */
static int build_patch(const uint8_t *cur, uint8_t *neu) {
    uint8_t reg;
    if (cur[0] != 0x0f || cur[1] != 0x44)
        return -1;
    if ((cur[2] & 0xc0) != 0xc0)
        return -1;
    reg = (uint8_t)((cur[2] >> 3) & 7);
    neu[0] = 0x31;
    neu[1] = (uint8_t)(0xc0 | (reg << 3) | reg);
    neu[2] = 0x90;
    return 0;
}

/* already patched: 31 /r 90 with mod==3 and reg==rm */
static int is_patched(const uint8_t *cur) {
    return cur[0] == 0x31 && cur[2] == 0x90 &&
           (cur[1] & 0xc0) == 0xc0 &&
           ((cur[1] >> 3) & 7) == (cur[1] & 7);
}

static int ctx_ok(const uint8_t *ctx) {
#if CTX_STRICT
    return !memcmp(ctx, CTX100, 64);
#else
    uint8_t tmp[3];
    return build_patch(ctx + CTX_OFF, tmp) == 0 || is_patched(ctx + CTX_OFF);
#endif
}

static const char WANT_TITLEID[] = "CUSA03041";

static int g_sock = -1;

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put64le(uint8_t *p, uint64_t v) {
    put32le(p, (uint32_t)v); put32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64le(const uint8_t *p) {
    return get32le(p) | ((uint64_t)get32le(p + 4) << 32);
}

/* read exactly n bytes or fail */
static int recvn(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(g_sock, (char *)buf + got, n - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int sendn(const uint8_t *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(g_sock, (const char *)buf + sent, n - sent, 0);
        if (r <= 0)
            return -1;
        sent += (size_t)r;
    }
    return 0;
}

static int cmd(uint32_t id, const uint8_t *body, uint32_t len) {
    uint8_t h[12];
    put32le(h, PACKET_MAGIC); put32le(h + 4, id); put32le(h + 8, len);
    if (sendn(h, 12))
        return -1;
    if (len && sendn(body, len))
        return -1;
    return 0;
}

static int expect_success(void) {
    uint8_t b[4];
    if (recvn(b, 4))
        return -1;
    return get32le(b) == WIRE_SUCCESS ? 0 : -1;
}

static int connect_dbg(void) {
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0)
        return -1;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(744);
    sa.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa))) {
        close(g_sock);
        g_sock = -1;
        return -1;
    }
    return 0;
}

static int notify(const char *msg) {
    uint32_t len = (uint32_t)strlen(msg) + 1;
    uint8_t b[8];
    char lb[128];
    int rc;
    put32le(b, 0); put32le(b + 4, len);
    if (cmd(CMD_CONSOLE_NOTIFY, b, 8)) {
        tlog("NOTIFY_SEND_FAIL");
        return -1;
    }
    /* trailing data: text + NUL */
    if (sendn((const uint8_t *)msg, len)) {
        tlog("NOTIFY_DATA_FAIL");
        return -1;
    }
    rc = expect_success();
    snprintf(lb, sizeof(lb), "NOTIFY rc=%d msg=%.64s", rc, msg);
    tlog(lb);
    return rc;
}

/* foreground app: pid + titleid. pid==0 when no game is in front. */
static int fg_app(uint32_t *pid, char titleid[16]) {
    uint8_t resp[140];
    if (cmd(CMD_FG_APP, NULL, 0))
        return -1;
    if (expect_success())
        return -1;
    if (recvn(resp, 140))
        return -1;
    *pid = get32le(resp);
    memcpy(titleid, resp + 4, 16);
    titleid[15] = 0;
    return 0;
}

static int proc_read(uint32_t pid, uint64_t addr, uint8_t *out, uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_READ, b, 16))
        return -1;
    if (expect_success())
        return -1;
    return recvn(out, len);
}

static int proc_write(uint32_t pid, uint64_t addr, const uint8_t *data, uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_WRITE, b, 16))
        return -1;
    if (expect_success())
        return -1;
    if (sendn(data, len))
        return -1;
    return expect_success();
}

/* local memmem in case the payload libc lacks it */
static void *local_memmem(const void *h, size_t hlen, const void *n, size_t nlen) {
    const uint8_t *hh = h;
    size_t i;
    if (!nlen || nlen > hlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hh + i, n, nlen))
            return (void *)(hh + i);
    return NULL;
}

/* base = start of the lowest RX 'executable' segment (home of .text) */
static int eboot_base(uint32_t pid, uint64_t *base) {
    uint8_t b[4], hdr[8];
    uint64_t best = 0;
    uint32_t num, i;
    put32le(b, pid);
    if (cmd(CMD_PROC_MAPS_X, b, 4))
        return -1;
    if (expect_success())
        return -1;
    if (recvn(hdr, 4))
        return -1;
    num = get32le(hdr);
    for (i = 0; i < num; i++) {
        uint8_t e[58];
        uint64_t start, end, sz;
        uint16_t prot;
        if (recvn(e, 58))
            return -1;
        start = get64le(e + 32); end = get64le(e + 40);
        sz = end - start;
        prot = (uint16_t)(e[56] | (e[57] << 8));
        if (!(prot & PROT_EXEC))
            continue;
        if (local_memmem(e, 32, "executable", 10) == NULL)
            continue;
        if (sz < 0x1000000) /* eboot .text is tens of MB */
            continue;
        if (best == 0 || start < best)
            best = start;
    }
    if (!best)
        return -1;
    *base = best;
    return 0;
}

/* one-shot flow: fast-path context check, MAPS fallback on mismatch.
 * Fast-path reads 64 B at fast_base+RVA-32; a match makes the target safe
 * without downloading MAPS (spawn->write ~1 s). Poll every 0.5 s. */
#define POLL_FAST_USEC 500000u
#define AUTOPATCH_VERSION "oneshot-1.1-v100"

int main(void) {
    uint32_t pid = 0;
    char titleid[16] = {0};
    uint64_t base = 0, target = 0;
    uint8_t ctx[64], cur[3], neu[3];
    int have_target = 0;
    unsigned waited = 0;
    char lb[128];
    const unsigned LIMIT = 1200; /* 1200 x 0.5 s = 10 min */

    printf("autopatch60 v%s (one-shot, RDR2 1.00): waiting for game...\n",
           AUTOPATCH_VERSION);
    tlog("BOOT oneshot v100");
    if (g_sock < 0 && connect_dbg()) {
        tlog("CONN_FAIL0");
        return 2;
    }
    tlog("CONN_OK");
    /* 1) wait for spawn */
    for (waited = 0; waited < LIMIT; waited++) {
        if (fg_app(&pid, titleid))
            break; /* dead connection: exit, nothing to patch */
        if (pid && !strcmp(titleid, WANT_TITLEID))
            break;
        usleep(POLL_FAST_USEC);
    }
    if (waited >= LIMIT || !pid || strcmp(titleid, WANT_TITLEID) != 0) {
        tlog("TIMEOUT_NO_GAME");
        return 0; /* quiet exit: nothing was ever launched */
    }
    snprintf(lb, sizeof(lb), "SPAWN pid=%u", pid);
    tlog(lb);

    /* 2) resolve target: fast-path at default base, MAPS fallback */
    target = IMG_BASE_DEFAULT + TARGET_RVA;
    if (!proc_read(pid, target - CTX_OFF, ctx, 64)) {
        log_hex("CTX64(fast)", ctx, 64);
        if (ctx_ok(ctx))
            have_target = 1;
    }
    if (!have_target && !eboot_base(pid, &base)) {
        target = base + TARGET_RVA;
        snprintf(lb, sizeof(lb), "BASE 0x%llx pid=%u",
                 (unsigned long long)base, pid);
        tlog(lb);
        if (!proc_read(pid, target - CTX_OFF, ctx, 64)) {
            log_hex("CTX64(maps)", ctx, 64);
            if (ctx_ok(ctx))
                have_target = 1;
        }
    }
    if (!have_target) {
        tlog("NO_TARGET");
        notify("RDR2 60fps: target not found, not applied");
        return 3;
    }

    /* 3) verify -> write -> re-verify -> notify -> exit */
    memcpy(cur, ctx + CTX_OFF, 3);
    if (is_patched(cur)) {
        tlog("ALREADY");
        notify("RDR2 60fps ON - made by KurohaXR");
        return 0;
    }
    if (build_patch(cur, neu) != 0) {
        snprintf(lb, sizeof(lb), "ABORT_UNEXP %02x%02x%02x pid=%u",
                 cur[0], cur[1], cur[2], pid);
        tlog(lb);
        notify("RDR2 60fps: unexpected eboot, not applied");
        return 4;
    }
    snprintf(lb, sizeof(lb), "WRITE_TRY %02x%02x%02x -> %02x%02x%02x",
             cur[0], cur[1], cur[2], neu[0], neu[1], neu[2]);
    tlog(lb);
    if (proc_write(pid, target, neu, 3) ||
        proc_read(pid, target, cur, 3) ||
        memcmp(cur, neu, 3) != 0) {
        tlog("WRITE_FAIL");
        notify("RDR2 60fps: write failed, not applied");
        return 5;
    }
    tlog("WRITE_OK");
    notify("RDR2 60fps ON - made by KurohaXR");
    printf("autopatch60: applied and verified, exiting.\n");
    return 0;
}
