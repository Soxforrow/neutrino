// p_gtfsstub.irx -- minimal RPC stub for Criterion's GTFSCDVD.IRX plus
// every other SIF RPC SID referenced by Black (SLUS_213.76).
//
// agent-N23 expansion: the previous build registered only the obvious
// Criterion / RWA SIDs. Static analysis of the game ELF (find_sids.py)
// now identifies 41 distinct SIDs the game's libsce calls into. ANY
// unregistered SID will block the game's sceSifBindRpc forever.
//
// Strategy: register a generic zero-return stub for EVERY SID the game
// references. Real IOP modules that register the same SID later in the
// boot sequence transparently override our stub (IOP RPC system uses
// last-registered), so we are a safety net, not a competitor.
//
// SID classification (from find_sids.py output, 2026-05-24):
//   System SIDs handled by real modules:
//     0x80000001 FILEIO            (wrapped by p_black)
//     0x80000002 IOPMEM/SYSMEM     (wrapped by p_black)
//     0x80000007 SIO2MAN           (real module preloaded)
//     0x80000009 MC2_D             (real module preloaded)
//   Criterion-internal:
//     0x00475453 GTFSCDVD ("\0GTS") - per-fno callback
//     0x004700xx-0x0047ffxx       - Criterion engine SIDs
//   Likely PS2SDK standard but possibly used by libsce:
//     0x80000003,06,08,0a,0c,0d,0e,0f,10,11,12,13,16,17,18,1a,1c,1e
//     0x80000006f, 0xa8, 0xc0
//     0x80000028,30,80,ae,af       (RWA)
//     0x800005xx                    (game-specific)

#include <loadcore.h>
#include <sifcmd.h>
#include <thbase.h>
#include <tamtypes.h>

#define MODNAME "p_gtfsstub"
IRX_ID(MODNAME, 1, 2);

#define GTFSCDVD_SID 0x00475453

// All SIDs to register stubs for. Order doesn't matter; first reg wins
// against later regs of the same SID inside this list, but a real
// module's later registration outside this list will override.
static const u32 stub_sids[] = {
    // ---- generic 0x800000xx range (game-referenced) ----
    0x80000003, 0x80000004, 0x80000006, 0x80000008,
    0x8000000a, 0x8000000c, 0x8000000d, 0x8000000e,
    0x8000000f, 0x80000010, 0x80000011, 0x80000012,
    0x80000013, 0x80000016, 0x80000017, 0x80000018,
    0x8000001a, 0x8000001c, 0x8000001e,
    // ---- libsd / less-common ----
    0x8000006f, 0x800000a8, 0x800000c0,
    // ---- RWA (RenderWare Audio) ----
    0x80000028, 0x80000030, 0x80000080, 0x800000ae, 0x800000af,
    // ---- game-specific (Black-only) ----
    0x80000592, 0x80000593, 0x8000059a, 0x8000059c,
    // ---- Criterion-internal 0x0047xxxx ----
    0x00470010, 0x00478b88, 0x00478c88, 0x00479490,
    0x00479518, 0x00479dc0, 0x0047a1c0, 0x0047a928,
    0x0047ea08, 0x0047fffc,
};
#define NUM_STUB_SIDS (sizeof(stub_sids) / sizeof(stub_sids[0]))

// One server data + reply buffer per SID. Aligned for SIF DMA.
static SifRpcServerData_t   sd_gtfs __attribute__((aligned(64)));
static u8 rpcbuf_gtfs[256]          __attribute__((aligned(64)));

static SifRpcServerData_t   sd_stub[NUM_STUB_SIDS] __attribute__((aligned(64)));
static u8 rpcbuf_stub[NUM_STUB_SIDS][256]          __attribute__((aligned(64)));

static SifRpcDataQueue_t    dq __attribute__((aligned(64)));

// GTFSCDVD callback - per-fno return codes that push game toward
// "clean error" rather than "infinite retry". Real module: 6 fnos
// (1=TOC, 2=no-op, 3=search, 4=open, 5=status, 6=close), each writes
// a single int result at *(int*)buf and returns buf.
static void *rpc_callback(int fno, void *buf, int size)
{
    int n = size > 64 ? 64 : size;
    int i;
    u8 *p = (u8 *)buf;
    for (i = 0; i < n; i++)
        p[i] = 0;

    int rc = 0;
    switch (fno) {
        case 1: rc =  0; break;
        case 2: rc =  0; break;
        case 3: rc = -1; break;
        case 4: rc = -1; break;
        case 5: rc =  0; break;
        case 6: rc =  0; break;
        default: rc = -1; break;
    }
    *(int *)buf = rc;
    return buf;
}

// Generic stub - zero the buffer, return success (rc=0 in slot 0).
static void *rpc_stub_callback(int fno, void *buf, int size)
{
    int n = size > 64 ? 64 : size;
    int i;
    u8 *p = (u8 *)buf;
    for (i = 0; i < n; i++)
        p[i] = 0;
    return buf;
}

// RPC server thread. Registers GTFSCDVD plus the entire stub_sids[]
// array on one shared DataQueue (one thread serves all).
static void rpc_server_thread(void *arg)
{
    unsigned int k;

    sceSifSetRpcQueue(&dq, GetThreadId());

    sceSifRegisterRpc(&sd_gtfs, GTFSCDVD_SID, &rpc_callback,
                      rpcbuf_gtfs, NULL, NULL, &dq);

    for (k = 0; k < NUM_STUB_SIDS; k++) {
        sceSifRegisterRpc(&sd_stub[k], stub_sids[k],
                          &rpc_stub_callback,
                          rpcbuf_stub[k], NULL, NULL, &dq);
    }

    sceSifRpcLoop(&dq);
}

int _start(int argc, char **argv)
{
    iop_thread_t th;
    int tid;

    (void)argc; (void)argv;

    sceSifInitRpc(0);

    th.attr     = TH_C;
    th.option   = 0;
    th.thread   = &rpc_server_thread;
    th.stacksize = 8192;     // bigger - registering many servers takes more
    th.priority = 0x30;

    tid = CreateThread(&th);
    if (tid < 0)
        return MODULE_NO_RESIDENT_END;

    StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}
