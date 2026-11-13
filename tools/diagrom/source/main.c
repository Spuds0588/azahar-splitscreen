// azahar-linktest — UDS local-wireless link diagnostic for azahar-splitscreen.
//
// Runs as a plain 3dsx homebrew (no game, no savedata) so the split-screen
// orchestrator can prove that N Azahar instances joined to one room can see
// each other over the emulated 3DS local-wireless (nwm::UDS) service.
//
//   A = host a network      B = join a network      START = quit
//
// The screen reports role, UDS connection state, the node bitmask, per-node
// packet counts, round-trip time and a LINK ACTIVE line once two or more
// nodes are on the network.
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WLANCOMM_ID 0x5A5A0001u // must match on every instance
#define NET_ID8 0
#define MAX_NODES 4
#define DATA_CHANNEL 1
#define HB_MAGIC 0x4C544B31u // "LTK1"
#define HB_INTERVAL_MS 100
#define STALL_MS 2500
#define RECV_BUF UDS_DEFAULT_RECVBUFSIZE
#define RTT_SLOTS 16

typedef struct {
    u32 magic;
    u16 node; // sender's NetworkNodeID
    u16 seq;
    u16 kind; // 0 = heartbeat, 1 = echo reply
    u16 pad;
    u32 uptime_ms;
} Packet;

enum { ROLE_NONE, ROLE_HOST, ROLE_CLIENT };

static udsBindContext bindctx;
static bool uds_ready;
static int role = ROLE_NONE;
static Result last_err;

static u16 seq_counter;
static u32 tx_count, rx_count;
static u32 rx_by_node[UDS_MAXNODES + 1];
static u32 send_ms[RTT_SLOTS];
static u32 last_rx_ms, start_ms;
static u32 rtt_last, rtt_min, rtt_max;
static Packet last_rx;
static u32 last_redraw_ms;
static u32 last_log_ms;
static FILE* log_file;

static u32 now_ms(void) {
    return (u32)osGetTime();
}

// Emits one line to the emulator log (svcOutputDebugString) so a headless run
// can be verified by grepping for "LT ". Harmless on real hardware.
static void log_line(const char* fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    const int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0) {
        return;
    }
    svcOutputDebugString(buf, len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1);
    if (log_file != NULL) {
        fputs(buf, log_file);
        fflush(log_file);
    }
}

static void usage(void) {
    consoleClear();
    printf("\x1b[1;1HAZAHAR LINKTEST v0.1\n\n");
    printf("UDS local-wireless diagnostic.\n");
    printf("Network id: %08lx  channel: %d  max nodes: %d\n\n", (unsigned long)WLANCOMM_ID,
           DATA_CHANNEL, MAX_NODES);
    printf("Join an Azahar room first (Multiplayer menu),\n");
    printf("then run this test in every instance.\n\n");
    printf("  A     host the test network (one instance)\n");
    printf("  B     join the test network (the others)\n");
    printf("  START quit\n");
}

static const char* uds_status_name(u32 status) {
    switch (status) {
    case 0:
        return "not connected";
    case 1:
        return "host";
    case 2:
        return "client";
    case 3:
        return "spectator";
    default:
        return "unknown";
    }
}

static void draw(void) {
    udsConnectionStatus st;
    memset(&st, 0, sizeof(st));
    const Result st_result = uds_ready ? udsGetConnectionStatus(&st) : (Result)-1;
    const u32 now = now_ms();

    // One-second rate window.
    static u32 rate_mark, rate_tx, rate_rx, rate_tx_prev, rate_rx_prev;
    if (now - rate_mark >= 1000) {
        rate_tx_prev = tx_count - rate_tx;
        rate_rx_prev = rx_count - rate_rx;
        rate_tx = tx_count;
        rate_rx = rx_count;
        rate_mark = now;
    }

    const bool linked = R_SUCCEEDED(st_result) && st.total_nodes > 1;
    const bool stalled = linked && last_rx_ms != 0 && now - last_rx_ms > STALL_MS;

    consoleClear();
    printf("\x1b[1;1HAZAHAR LINKTEST v0.1   %s\n\n",
           role == ROLE_HOST ? "HOST" : role == ROLE_CLIENT ? "JOINER" : "IDLE");

    if (!uds_ready) {
        printf("udsInit failed: 0x%08lx\n\n", (unsigned long)last_err);
        printf("START quit.\n");
        return;
    }

    if (R_FAILED(st_result)) {
        printf("udsGetConnectionStatus failed: 0x%08lx\n", (unsigned long)st_result);
    } else {
        printf("uds: %-12s my node %u  nodes %u/%u  mask 0x%04x\n",
               uds_status_name(st.status), st.cur_NetworkNodeID, st.total_nodes, st.max_nodes,
               st.node_bitmask);
    }
    printf("tx %lu (%lu/s)  rx %lu (%lu/s)\n", (unsigned long)tx_count,
           (unsigned long)rate_tx_prev, (unsigned long)rx_count, (unsigned long)rate_rx_prev);

    printf("peers:");
    bool any_peer = false;
    for (u32 i = 1; i <= UDS_MAXNODES; ++i) {
        if (rx_by_node[i] != 0) {
            printf(" n%lu:%lu", (unsigned long)i, (unsigned long)rx_by_node[i]);
            any_peer = true;
        }
    }
    if (!any_peer) {
        printf(" none yet");
    }
    printf("\n");

    if (rx_count != 0) {
        printf("last: node %u seq %lu kind %u\n", last_rx.node, (unsigned long)last_rx.seq,
               last_rx.kind);
    } else {
        printf("last: -\n");
    }

    if (rtt_last != 0) {
        printf("rtt: %lu ms (min %lu, max %lu)\n", (unsigned long)rtt_last,
               (unsigned long)rtt_min, (unsigned long)rtt_max);
    } else {
        printf("rtt: -\n");
    }

    printf("last error: 0x%08lx\n\n", (unsigned long)last_err);

    if (stalled) {
        printf("STALL - no packets for %lu ms\n", (unsigned long)(now - last_rx_ms));
    } else if (linked) {
        printf("LINK ACTIVE - %u PS\n", st.total_nodes);
    } else {
        printf("waiting for peers...\n");
    }

    printf("\nSTART quit.\n");

    // One machine-readable line per second for log-based verification.
    if (now - last_log_ms >= 1000) {
        last_log_ms = now;
        u32 peers_mask = 0;
        for (u32 i = 1; i <= UDS_MAXNODES; ++i) {
            if (rx_by_node[i] != 0) {
                peers_mask |= 1u << i;
            }
        }
        log_line("LT role=%s state=%lu node=%u nodes=%u/%u mask=%04x peers=%02x tx=%lu rx=%lu "
                 "rtt=%lu/%lu/%lu link=%s\n",
                 role == ROLE_HOST ? "host" : role == ROLE_CLIENT ? "join" : "idle",
                 (unsigned long)st.status, st.cur_NetworkNodeID, st.total_nodes, st.max_nodes,
                 st.node_bitmask, peers_mask, (unsigned long)tx_count, (unsigned long)rx_count,
                 (unsigned long)rtt_last, (unsigned long)rtt_min, (unsigned long)rtt_max,
                 stalled ? "stall" : linked ? "active" : "waiting");
    }
}

static void drain_packets(void) {
    for (int i = 0; i < 32; ++i) {
        Packet pkt;
        size_t actual = 0;
        u16 src = 0;
        memset(&pkt, 0, sizeof(pkt));

        const Result res = udsPullPacket(&bindctx, &pkt, sizeof(pkt), &actual, &src);
        if (R_FAILED(res)) {
            last_err = res;
            return;
        }
        if (actual < sizeof(pkt) || pkt.magic != HB_MAGIC) {
            return;
        }

        ++rx_count;
        if (src <= UDS_MAXNODES) {
            ++rx_by_node[src];
        }
        last_rx = pkt;
        last_rx_ms = now_ms();

        if (pkt.kind == 0 && role != ROLE_NONE) {
            // Echo heartbeats back to the sender so both ends can measure RTT.
            Packet echo = pkt;
            echo.kind = 1;
            echo.node = (u16)(pkt.node); // informational; src is authoritative
            const Result echo_res = udsSendTo(src, DATA_CHANNEL, UDS_SENDFLAG_Default, &echo,
                                              sizeof(echo));
            if (R_FAILED(echo_res)) {
                last_err = echo_res;
            }
        } else if (pkt.kind == 1) {
            const u32 slot = pkt.seq % RTT_SLOTS;
            const u32 rtt = now_ms() - send_ms[slot];
            rtt_last = rtt;
            if (rtt_min == 0 || rtt < rtt_min) {
                rtt_min = rtt;
            }
            if (rtt > rtt_max) {
                rtt_max = rtt;
            }
        }
    }
}

static void service_tick(void) {
    const u32 now = now_ms();

    if (now - start_ms >= HB_INTERVAL_MS) {
        start_ms = now;
        Packet hb;
        memset(&hb, 0, sizeof(hb));
        hb.magic = HB_MAGIC;
        hb.node = 0; // filled in by the reader from the actual source node
        hb.seq = ++seq_counter;
        hb.kind = 0;
        hb.uptime_ms = now;

        send_ms[hb.seq % RTT_SLOTS] = now;
        const Result res = udsSendTo(UDS_BROADCAST_NETWORKNODEID, DATA_CHANNEL,
                                    UDS_SENDFLAG_Default, &hb, sizeof(hb));
        if (R_FAILED(res)) {
            last_err = res;
        } else {
            ++tx_count;
        }
    }

    drain_packets();

    if (now - last_redraw_ms >= 250) {
        last_redraw_ms = now;
        draw();
    }
}

static void teardown(void) {
    if (!uds_ready) {
        return;
    }
    if (role == ROLE_HOST) {
        udsDestroyNetwork();
    } else if (role == ROLE_CLIENT) {
        udsDisconnectNetwork();
    }
    udsUnbind(&bindctx);
    udsExit();
    uds_ready = false;
    role = ROLE_NONE;
}

static bool start_uds(void) {
    memset(&bindctx, 0, sizeof(bindctx));
    last_err = udsInit(0x4000, NULL);
    uds_ready = R_SUCCEEDED(last_err);
    return uds_ready;
}

static void reset_counters(void) {
    tx_count = rx_count = 0;
    memset(rx_by_node, 0, sizeof(rx_by_node));
    memset(send_ms, 0, sizeof(send_ms));
    rtt_last = rtt_min = rtt_max = 0;
    last_rx_ms = 0;
    last_rx = (Packet){0};
    start_ms = now_ms();
    seq_counter = 0;
}

static void do_host(void) {
    if (!start_uds()) {
        return;
    }
    reset_counters();

    udsNetworkStruct net;
    udsGenerateDefaultNetworkStruct(&net, WLANCOMM_ID, NET_ID8, MAX_NODES);

    last_err = udsCreateNetwork(&net, NULL, 0, &bindctx, DATA_CHANNEL, RECV_BUF);
    if (R_FAILED(last_err)) {
        log_line("LT host-failed code=%08lx\n", (unsigned long)last_err);
        udsExit();
        uds_ready = false;
        return;
    }
    role = ROLE_HOST;
    log_line("LT hosting wlancomm=%08lx max_nodes=%d\n", (unsigned long)WLANCOMM_ID, MAX_NODES);
}

static void do_join(void) {
    if (!start_uds()) {
        return;
    }
    reset_counters();

    static u8 scanbuf[0x8000] __attribute__((aligned(0x1000)));
    udsNetworkScanInfo* networks = NULL;
    size_t total = 0;

    // Beacons arrive every ~102 ms; scan for a few seconds before giving up.
    for (int attempt = 0; attempt < 10; ++attempt) {
        draw();
        total = 0;
        networks = NULL;
        last_err = udsScanBeacons(scanbuf, sizeof(scanbuf), &networks, &total, WLANCOMM_ID,
                                  NET_ID8, NULL, false);
        if (R_SUCCEEDED(last_err) && total > 0) {
            break;
        }
        svcSleepThread(500ULL * 1000 * 1000);
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            udsExit();
            uds_ready = false;
            return;
        }
    }

    if (R_FAILED(last_err) || total == 0 || networks == NULL) {
        free(networks);
        udsExit();
        uds_ready = false;
        if (R_SUCCEEDED(last_err)) {
            last_err = (Result)-2; // scanned cleanly, found nothing
        }
        log_line("LT join-no-network code=%08lx found=%lu\n", (unsigned long)last_err,
                 (unsigned long)total);
        return;
    }

    last_err = udsConnectNetwork(&networks[0].network, NULL, 0, &bindctx,
                                 UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, DATA_CHANNEL,
                                 RECV_BUF);
    free(networks);
    if (R_FAILED(last_err)) {
        log_line("LT join-failed code=%08lx\n", (unsigned long)last_err);
        udsExit();
        uds_ready = false;
        return;
    }
    role = ROLE_CLIENT;
    log_line("LT joined network\n");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // sdmc:/linktest.log is the machine-readable copy (survives log filters).
    fsInit();
    log_file = fopen("sdmc:/linktest.log", "w");
    log_line("LT boot wlancomm=%08lx max_nodes=%d\n", (unsigned long)WLANCOMM_ID, MAX_NODES);

    usage();

    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys = hidKeysDown();
        if (keys & KEY_START) {
            break;
        }

        if (role == ROLE_NONE) {
            if (keys & KEY_A) {
                do_host();
                if (role != ROLE_NONE) {
                    draw();
                } else {
                    usage();
                    printf("\nhost failed: 0x%08lx\n", (unsigned long)last_err);
                }
            } else if (keys & KEY_B) {
                do_join();
                if (role != ROLE_NONE) {
                    draw();
                } else {
                    usage();
                    printf("\njoin failed: 0x%08lx\n", (unsigned long)last_err);
                }
            }
        } else {
            service_tick();
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        svcSleepThread(16ULL * 1000 * 1000); // ~60 Hz
    }

    teardown();
    gfxExit();
    return 0;
}
