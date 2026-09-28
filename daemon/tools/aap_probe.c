/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * aap-probe: standalone AAP sniffer / command sender for protocol checks.
 * Stop earport-daemon first (only one AAP link at a time).
 *
 *   aap-probe ADDR [cmd ...]
 *     wait N            listen N seconds
 *     set ID HEX...     control command 04 00 04 00 09 00 ID v0 v1 v2 v3
 *     raw HEX...        send raw bytes
 *     rename NAME       RENAME packet (opcode 0x1A)
 *     mark TEXT         print a marker line
 * Ends with a 3 s listen.
 */
#define _POSIX_C_SOURCE 200809L

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int sock;
static struct timespec t0;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static const char *ctrl_name(unsigned id)
{
    switch (id) {
    case 0x01: return "MIC_MODE";
    case 0x0D: return "LISTENING_MODE";
    case 0x16: return "CLICK_HOLD_MODE";
    case 0x17: return "DOUBLE_CLICK_INTERVAL";
    case 0x18: return "CLICK_HOLD_INTERVAL";
    case 0x1A: return "LISTENING_MODE_CONFIGS";
    case 0x1B: return "ONE_BUD_ANC_MODE";
    case 0x1F: return "CHIME_VOLUME";
    case 0x23: return "VOLUME_SWIPE_INTERVAL";
    case 0x24: return "CALL_MANAGEMENT_CONFIG";
    case 0x25: return "VOLUME_SWIPE_MODE";
    case 0x26: return "ADAPTIVE_VOLUME_CONFIG";
    case 0x28: return "CONVERSATION_DETECT";
    case 0x29: return "SSL";
    case 0x2C: return "HEARING_AID";
    case 0x2E: return "AUTO_ANC_STRENGTH";
    case 0x2F: return "HPS_GAIN_SWIPE";
    case 0x33: return "HEARING_ASSIST_CONFIG";
    case 0x34: return "ALLOW_OFF_OPTION";
    case 0x35: return "SLEEP_DETECTION_CONFIG";
    case 0x39: return "STEM_CONFIG";
    default:   return "?";
    }
}

static void dump(const char *dir, const unsigned char *b, ssize_t n)
{
    printf("[%7.3f] %s:", now(), dir);
    for (ssize_t i = 0; i < n; i++)
        printf(" %02X", b[i]);
    if (n >= 8 && b[0] == 4 && b[2] == 4 && b[4] == 0x09)
        printf("   <ctrl %s>", ctrl_name(b[6]));
    else if (n >= 6 && b[0] == 4 && b[2] == 4)
        printf("   <opcode 0x%02X>", b[4]);
    printf("\n");
    fflush(stdout);
}

static void send_pkt(const unsigned char *b, size_t n)
{
    dump("TX", b, n);
    if (send(sock, b, n, 0) < 0)
        perror("send");
}

static void listen_for(double secs)
{
    double end = now() + secs;
    unsigned char buf[1024];
    while (now() < end) {
        struct pollfd p = { .fd = sock, .events = POLLIN };
        int ms = (int)((end - now()) * 1000);
        if (ms <= 0 || poll(&p, 1, ms) <= 0)
            continue;
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            printf("[%7.3f] link closed\n", now());
            exit(1);
        }
        dump("RX", buf, n);
    }
}

static size_t parse_hex(int argc, char **argv, unsigned char *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < argc && n < max; i++)
        out[n++] = (unsigned char)strtoul(argv[i], NULL, 16);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s ADDR [cmd ...]\n", argv[0]);
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct sockaddr_l2 addr = { 0 };
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(0x1001);
    str2ba(argv[1], &addr.l2_bdaddr);

    sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0 || connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    const unsigned char handshake[] = { 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                        0, 0, 0, 0, 0, 0, 0, 0 };
    const unsigned char features[] = { 0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00,
                                       0, 0, 0, 0, 0, 0 };
    const unsigned char notif[] = { 0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };

    send_pkt(handshake, sizeof(handshake));
    listen_for(0.1);
    send_pkt(features, sizeof(features));
    listen_for(0.1);
    send_pkt(notif, sizeof(notif));
    listen_for(2.0);

    /* Split argv into commands: each starts with a keyword */
    int i = 2;
    while (i < argc) {
        int j = i + 1;
        while (j < argc && strcmp(argv[j], "wait") && strcmp(argv[j], "set") &&
               strcmp(argv[j], "raw") && strcmp(argv[j], "rename") && strcmp(argv[j], "mark"))
            j++;
        int nargs = j - i - 1;
        char **args = argv + i + 1;

        if (!strcmp(argv[i], "wait") && nargs == 1) {
            listen_for(atof(args[0]));
        } else if (!strcmp(argv[i], "mark")) {
            printf("[%7.3f] ---- %s ----\n", now(), nargs ? args[0] : "");
        } else if (!strcmp(argv[i], "set") && nargs >= 2) {
            unsigned char pkt[11] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00 };
            pkt[6] = (unsigned char)strtoul(args[0], NULL, 16);
            parse_hex(nargs - 1, args + 1, pkt + 7, 4);
            send_pkt(pkt, sizeof(pkt));
            listen_for(1.0);
        } else if (!strcmp(argv[i], "raw") && nargs >= 1) {
            unsigned char pkt[256];
            size_t n = parse_hex(nargs, args, pkt, sizeof(pkt));
            send_pkt(pkt, n);
            listen_for(1.0);
        } else if (!strcmp(argv[i], "rename") && nargs == 1) {
            unsigned char pkt[256] = { 0x04, 0x00, 0x04, 0x00, 0x1A, 0x00, 0x01 };
            size_t len = strlen(args[0]);
            if (len > 32)
                len = 32;
            pkt[7] = (unsigned char)len;
            pkt[8] = 0x00;
            memcpy(pkt + 9, args[0], len);
            send_pkt(pkt, 9 + len);
            listen_for(1.0);
        } else {
            fprintf(stderr, "bad command at '%s'\n", argv[i]);
            return 2;
        }
        i = j;
    }

    listen_for(3.0);
    close(sock);
    return 0;
}
