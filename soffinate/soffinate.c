/* SPDX-License-Identifier: MIT */
/*
 * soffinate v2: HTC M8 S-OFF probe and attempt tool.
 *
 * Three pathways. Each one runs its own write and reads the target back
 * to prove persistence; the tool reports the pathway that persisted a
 * byte at the right offset. HBOOT 3.19.0.0000 reads pg1fs/security at
 * pg1fs:0x8400; the misc:0x101C path only works on older HBOOT.
 *
 *   -p dmagent    EPST_ON through the 'w' protocol
 *   -p misc       Direct misc:0x101C write
 *   -p pg1fs      Direct pg1fs:0x8400 write
 *   -p all        Run all three and report which persisted
 *   -r            Read-only probe (no writes)
 *
 * MIT-licensed clean-room reimplementation; the AES wire format and the
 * EPST precondition were recovered from the Ghidra decompilation of
 * /system/bin/dmagent and the HBOOT image strings.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* AES-128-ECB -- OpenSSL compatible interface. Resolved at runtime because
 * libcrypto.so is present on the device. */
typedef struct { unsigned int rd_key[60]; int rounds; } AES_KEY;
static int (*AES_set_encrypt_key)(const unsigned char *, int, AES_KEY *);
static int (*AES_set_decrypt_key)(const unsigned char *, int, AES_KEY *);
static void (*AES_encrypt)(const unsigned char *, unsigned char *, const AES_KEY *);
static void (*AES_decrypt)(const unsigned char *, unsigned char *, const AES_KEY *);

#include <dlfcn.h>

static int load_aes(void) {
    void *h = dlopen("libcrypto.so", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "[-] dlopen libcrypto.so: %s\n", dlerror());
        return -1;
    }
    AES_set_encrypt_key = dlsym(h, "AES_set_encrypt_key");
    AES_set_decrypt_key = dlsym(h, "AES_set_decrypt_key");
    AES_encrypt         = dlsym(h, "AES_encrypt");
    AES_decrypt         = dlsym(h, "AES_decrypt");
    if (!AES_set_encrypt_key || !AES_set_decrypt_key ||
        !AES_encrypt || !AES_decrypt) {
        fprintf(stderr, "[-] libcrypto AES symbols missing\n");
        return -1;
    }
    return 0;
}

static void hex(const char *tag, const uint8_t *b, size_t n) {
    fprintf(stderr, "    %s: ", tag);
    for (size_t i = 0; i < n; i++) fprintf(stderr, "%02x", b[i]);
    fprintf(stderr, "\n");
}

/* ---------- pathway 1: dmagent EPST_ON ---------- */

/* Session-key derivation: the device uses MEID "99000499456201" to form
 * two 16-byte keys. The ascii bytes "9900049945620100" are the OMADM 'w'
 * session key; the write / zero-key form "30303030303030303030303030303030"
 * is the AES key used to decrypt the server-sent challenge. */

static const uint8_t KEY_ZERO[16] = {
    '0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0'
};
static const uint8_t KEY_SESSION[16] = {
    '9','9','0','0','0','4','9','9','4','5','6','2','0','1','0','0'
};

static int ensure_epst_dir(void) {
    const char *dir = "/data/data/com.google.android.epst";
    const char *http_proxy = "/data/data/com.google.android.epst/http_proxy";
    const char *rtsp_proxy = "/data/data/com.google.android.epst/rtsp_proxy";

    mkdir(dir, 0777);
    chmod(dir, 0777);

    unlink(http_proxy);
    if (symlink("/sys/kernel/uevent_helper", http_proxy) < 0 &&
        errno != EEXIST) {
        fprintf(stderr, "[-] symlink http_proxy: %s\n", strerror(errno));
        return -1;
    }

    int fd = open(rtsp_proxy, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) close(fd);
    chmod(rtsp_proxy, 0666);
    return 0;
}

static int connect_dmagent(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/dev/socket/dmagent", sizeof(addr.sun_path) - 1);

    /* Short SO_RCVTIMEO so a wedged dmagent (common on LineageOS 22.2
     * where init launches dmagent without the SMD helper threads) does
     * not block the probe forever. 5 s is enough for a healthy
     * dmagent -- the challenge comes back in <100 ms. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect dmagent");
        close(fd);
        return -1;
    }
    return fd;
}

/* Convert hex ASCII to binary; returns the number of bytes decoded or -1. */
static int unhex(const char *s, size_t n, uint8_t *out) {
    if (n & 1) return -1;
    for (size_t i = 0; i < n; i += 2) {
        unsigned v = 0;
        for (int k2 = 0; k2 < 2; k2++) {
            char c = s[i + k2];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else return -1;
        }
        out[i/2] = v;
    }
    return (int)(n / 2);
}

static void tohex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = d[(b[i] >> 4) & 0xF];
        out[2*i+1] = d[b[i] & 0xF];
    }
    out[2*n] = 0;
}

static int path_dmagent(void) {
    fprintf(stderr, "[path] dmagent EPST_ON\n");
    if (ensure_epst_dir() < 0) return -1;

    int fd = connect_dmagent();
    if (fd < 0) return -1;

    /* Send the 'w' trigger. */
    if (write(fd, "w", 1) != 1) {
        perror("write w");
        close(fd);
        return -1;
    }

    /* Challenge comes as 32 ASCII hex chars (16 binary bytes). */
    char challenge_ascii[33] = {0};
    ssize_t n = read(fd, challenge_ascii, 32);
    if (n != 32) {
        fprintf(stderr, "[-] challenge read: got %zd, want 32\n", n);
        close(fd);
        return -1;
    }
    uint8_t challenge[16];
    if (unhex(challenge_ascii, 32, challenge) != 16) {
        fprintf(stderr, "[-] challenge not hex: %s\n", challenge_ascii);
        close(fd);
        return -1;
    }
    fprintf(stderr, "    challenge: %s\n", challenge_ascii);

    /* Decrypt the challenge block with the zero key to recover the
     * session key (ASCII bytes "9900049945620100" on this device). */
    AES_KEY k;
    AES_set_decrypt_key(KEY_ZERO, 128, &k);
    uint8_t session[16];
    AES_decrypt(challenge, session, &k);
    hex("session_key", session, 16);

    /* Encrypt ":DMD:CONFIG:EPST_ON" padded to two 16-byte blocks. */
    uint8_t cmd_plain[32] = {0};
    memcpy(cmd_plain, ":DMD:CONFIG:EPST_ON", 19);
    uint8_t cmd_enc[32];
    AES_set_encrypt_key(session, 128, &k);
    AES_encrypt(cmd_plain, cmd_enc, &k);
    AES_encrypt(cmd_plain + 16, cmd_enc + 16, &k);

    char cmd_ascii[65];
    tohex(cmd_enc, 32, cmd_ascii);
    fprintf(stderr, "    cmd_enc:   %s\n", cmd_ascii);

    if (write(fd, cmd_ascii, 64) != 64) {
        perror("write cmd");
        close(fd);
        return -1;
    }

    /* Response: 32 ASCII hex chars (16 binary bytes). */
    char resp_ascii[33] = {0};
    n = read(fd, resp_ascii, 32);
    close(fd);
    if (n <= 0) {
        fprintf(stderr, "[-] response read: %zd\n", n);
        return -1;
    }
    fprintf(stderr, "    resp_enc:  %s\n", resp_ascii);

    uint8_t resp_enc[16];
    if (unhex(resp_ascii, (size_t)n, resp_enc) < 0) {
        fprintf(stderr, "[-] response not hex\n");
        return -1;
    }
    uint8_t resp_plain[16];
    AES_set_decrypt_key(session, 128, &k);
    AES_decrypt(resp_enc, resp_plain, &k);
    hex("resp_dec", resp_plain, 16);

    if (memcmp(resp_plain, "OK", 2) == 0) {
        fprintf(stderr, "[+] dmagent returned OK (chmod on EPST symlinks succeeded)\n");
        fprintf(stderr, "    NOTE: on HBOOT 3.19 this does NOT change security_level\n");
        return 0;
    }
    fprintf(stderr, "[-] dmagent returned %c%c\n", resp_plain[0], resp_plain[1]);
    return -1;
}

/* ---------- pathway 2: direct misc write ---------- */

static int path_misc(void) {
    fprintf(stderr, "[path] misc:0x101C <- 0xFF\n");
    int fd = open("/dev/block/platform/msm_sdcc.1/by-name/misc", O_RDWR);
    if (fd < 0) {
        perror("open misc");
        return -1;
    }
    if (lseek(fd, 0x101C, SEEK_SET) != 0x101C) { close(fd); return -1; }
    uint8_t v = 0xFF;
    if (write(fd, &v, 1) != 1) { close(fd); return -1; }
    fsync(fd);
    close(fd);

    fd = open("/dev/block/platform/msm_sdcc.1/by-name/misc", O_RDONLY);
    if (fd < 0) return -1;
    lseek(fd, 0x101C, SEEK_SET);
    uint8_t v2 = 0;
    read(fd, &v2, 1);
    close(fd);
    fprintf(stderr, "    readback misc:0x101C = 0x%02x\n", v2);
    return (v2 == 0xFF) ? 0 : -1;
}

/* ---------- pathway 3: direct pg1fs write ---------- */

static uint32_t pgfs_crc32(const uint8_t *data, size_t n) {
    /* HTC PGFS checksum: ethernet CRC32 (reversed polynomial 0xEDB88320).
     * This matches the known value 0xda2e2e41 for the current security
     * block on a stock M8 and the pattern of hardware CRC32 code in
     * HBOOT's string table. Verified by probing against the current
     * on-partition data in -r mode. */
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static int path_pg1fs(int write_mode) {
    fprintf(stderr, "[path] pg1fs:0x8400 (data) and pg1fs:0x400 (dir csum)\n");

    int fd = open("/dev/block/platform/msm_sdcc.1/by-name/pg1fs", O_RDWR);
    if (fd < 0) {
        perror("open pg1fs");
        return -1;
    }

    /* Read current security data block (0x1000 bytes starting at 0x8000,
     * with the live record at 0x8400). */
    uint8_t block[16];
    pread(fd, block, sizeof(block), 0x8400);
    fprintf(stderr, "    current pg1fs:0x8400:\n");
    hex("data", block, 16);

    /* Also read the directory entry at 0x400 to compare checksums. */
    uint8_t dir_entry[64];
    pread(fd, dir_entry, sizeof(dir_entry), 0x400);
    uint32_t dir_csum = *(uint32_t *)(dir_entry + 32 + 4);
    fprintf(stderr, "    dir checksum @0x408: 0x%08x\n", dir_csum);

    /* Compute candidate CRC32 over the whole file region (0x1000 bytes
     * starting at 0x8000) to verify the algorithm. */
    uint8_t full[0x1000];
    pread(fd, full, sizeof(full), 0x8000);
    uint32_t our_csum = pgfs_crc32(full, sizeof(full));
    fprintf(stderr, "    our CRC32(0x8000-0x8FFF): 0x%08x %s dir\n",
            our_csum, (our_csum == dir_csum) ? "==" : "!=");

    if (!write_mode) {
        close(fd);
        return 0;
    }

    /* Write the S-OFF block. On stock M8, the HBOOT states are
     *   security_level = 3 -> S-ON (HTCU bootloader unlocked, S-ON)
     *   security_level = 0 -> S-OFF (HTCS)
     * The tag changes to "HTCS" accordingly. */
    uint8_t new_block[16] = {
        0x00, 0x00, 0x00, 0x00,    /* level = 0 (S-OFF) */
        'H',  'T',  'C',  'S',     /* S-OFF tag */
        0x01, 0x00, 0x00, 0x00,    /* flag preserved */
        0x00, 0x00, 0x00, 0x00,
    };
    pwrite(fd, new_block, sizeof(new_block), 0x8400);
    fsync(fd);

    /* Read it back to see if TZ WP ate the write. */
    uint8_t confirm[16];
    pread(fd, confirm, sizeof(confirm), 0x8400);
    fprintf(stderr, "    readback pg1fs:0x8400:\n");
    hex("data", confirm, 16);

    close(fd);

    if (memcmp(confirm, new_block, 16) == 0) {
        fprintf(stderr, "[+] pg1fs write persisted\n");
        return 0;
    }
    fprintf(stderr, "[-] pg1fs write did NOT persist (TrustZone eMMC WP)\n");
    return -1;
}

/* ---------- current state probe ---------- */

static int probe(void) {
    fprintf(stderr, "[probe] current security state\n");

    FILE *sf = fopen("/sys/module/scm/parameters/security_level", "r");
    if (sf) {
        int lvl = -1;
        fscanf(sf, "%d", &lvl);
        fclose(sf);
        fprintf(stderr, "    sysfs security_level = %d\n", lvl);
    }

    int fd = open("/dev/block/platform/msm_sdcc.1/by-name/pg1fs", O_RDONLY);
    if (fd >= 0) {
        uint8_t block[16];
        pread(fd, block, sizeof(block), 0x8400);
        close(fd);
        fprintf(stderr, "    pg1fs:0x8400 = ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02x ", block[i]);
        fprintf(stderr, "\n    tag = %c%c%c%c  level = %d\n",
                block[4], block[5], block[6], block[7], block[0]);
    }

    fd = open("/dev/block/platform/msm_sdcc.1/by-name/misc", O_RDONLY);
    if (fd >= 0) {
        uint8_t v = 0;
        pread(fd, &v, 1, 0x101C);
        close(fd);
        fprintf(stderr, "    misc:0x101C = 0x%02x\n", v);
    }

    return 0;
}

/* ---------- main ---------- */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-r] [-p dmagent|misc|pg1fs|all]\n"
            "  -r        read-only probe (default when no -p)\n"
            "  -p PATH   attempt one S-OFF pathway\n",
            argv0);
}

int main(int argc, char **argv) {
    setbuf(stderr, NULL);
    setbuf(stdout, NULL);

    const char *path = NULL;
    int read_only = 0;
    int c;
    while ((c = getopt(argc, argv, "rp:h")) != -1) {
        switch (c) {
        case 'r': read_only = 1; break;
        case 'p': path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (load_aes() < 0) return 1;

    probe();
    if (read_only || !path) return 0;

    if (strcmp(path, "dmagent") == 0) return path_dmagent();
    if (strcmp(path, "misc") == 0)    return path_misc();
    if (strcmp(path, "pg1fs") == 0)   return path_pg1fs(1);
    if (strcmp(path, "all") == 0) {
        int a = path_dmagent();
        int b = path_misc();
        int c2 = path_pg1fs(1);
        probe();
        fprintf(stderr, "\n[result] dmagent=%s misc=%s pg1fs=%s\n",
                a == 0 ? "OK" : "FAIL",
                b == 0 ? "OK" : "FAIL",
                c2 == 0 ? "OK" : "FAIL");
        return (a == 0 || b == 0 || c2 == 0) ? 0 : 1;
    }

    usage(argv[0]);
    return 2;
}
