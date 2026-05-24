// p_gtfsstub.irx — minimal RPC stub for Criterion's GTFSCDVD.IRX
//
// Black (SLUS_213.76) and other Criterion engine titles ship a custom IOP
// module "gtfsdvd" (GTFSCDVD.IRX) that registers a SIF RPC server with
// SID 0x00475453 (ASCII: \0GTS, a Criterion-internal identifier).
// The game's EE-side code calls into this RPC to perform disc directory
// lookups, file opens, and streaming reads.
//
// On PS2 SCPH-7000x V12 silicon, neutrino's setup loads cdvdman_emu (a
// network-streaming shim) in place of Sony's stock cdvdman. The real
// GTFSCDVD.IRX, when SifExecModuleBuffer'd onto this IOP, hangs in its
// _start() / worker thread - presumably because of either large bss
// allocation pressure, or because its sceCdRead callbacks interact with
// cdvdman_emu's async-via-RPC pattern in a way that deadlocks.
//
// Rather than load the real GTFSCDVD, we substitute this minimal stub.
// It registers SID 0x00475453 with a callback that simply zero-fills the
// reply buffer and returns it. The real GTFSCDVD's RPC server callback
// (per agent RE analysis) has 6 fnos (1..6) all of which write a single
// int result at bss+0x60 and return its address. Fno 2 is a documented
// no-op in the real module.
//
// Returning 0 for every fno lets the game's EE-side sceSifCallRpc waits
// complete with "success, returned 0". Game's read-from-disc code that
// would have proceeded based on directory data will now get back zero
// (meaning "0 bytes read" or "EOF"), but at least it won't HANG. From
// there the game might either:
//   (a) handle the zero-result gracefully and continue with a fallback,
//   (b) give up gracefully with an error message,
//   (c) progress as far as possible before hitting the next blocker.
// Any of these is forward progress vs the current hang in sceSifCallRpc.

#include <loadcore.h>
#include <sifcmd.h>
#include <thbase.h>
#include <tamtypes.h>

#define MODNAME "p_gtfsstub"
IRX_ID(MODNAME, 1, 1);

#define GTFSCDVD_SID 0x00475453

// RPC server data and queue
static SifRpcServerData_t   sd __attribute__((aligned(64)));
static SifRpcDataQueue_t    dq __attribute__((aligned(64)));

// Reply buffer. The real GTFSCDVD's largest reply is ~32 bytes (a directory
// entry result struct). 256 bytes gives us comfortable headroom and matches
// typical SIF RPC buffer sizes.
static u8 rpcbuf[256] __attribute__((aligned(64)));

// RPC server callback. Real GTFSCDVD callback dispatches on fno 1..6 and
// writes a single int result at *(int*)buf. We mimic that: zero the entire
// reply buffer and return it. The leading int is 0, which means "success
// with no data" for the game's expected reply layout.
static void *rpc_callback(int fno, void *buf, int size)
{
    // Zero the buffer (or at least the first 64 bytes - whichever is less).
    // Most importantly, *(int*)buf = 0 which is the slot the real callback
    // uses for its function-result code.
    int n = size > 64 ? 64 : size;
    int i;
    u8 *p = (u8 *)buf;
    for (i = 0; i < n; i++)
        p[i] = 0;
    return buf;
}

// RPC server thread. SifSetRpcQueue/RegisterRpc/RpcLoop is the standard
// IOP RPC server pattern - see e.g. neutrino's cdvdfsv.c.
static void rpc_server_thread(void *arg)
{
    sceSifSetRpcQueue(&dq, GetThreadId());
    sceSifRegisterRpc(&sd, GTFSCDVD_SID, &rpc_callback, rpcbuf,
                      NULL, NULL, &dq);
    sceSifRpcLoop(&dq);
    // Never returns. If it did somehow, the thread terminates and the RPC
    // service becomes unavailable - but neither outcome is worse than the
    // current state where no service exists at all.
}

int _start(int argc, char **argv)
{
    iop_thread_t th;
    int tid;

    (void)argc; (void)argv;

    // Make sure SIF RPC subsystem is initialized. Safe to call repeatedly.
    sceSifInitRpc(0);

    // Spawn the RPC server thread.
    th.attr     = TH_C;       // C-callable thread
    th.option   = 0;
    th.thread   = &rpc_server_thread;
    th.stacksize = 4096;
    th.priority = 0x30;       // typical IOP service-thread priority

    tid = CreateThread(&th);
    if (tid < 0)
        return MODULE_NO_RESIDENT_END;

    StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}
