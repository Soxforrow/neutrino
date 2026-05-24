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

// Additional SIDs the game ELF references but no IOP module necessarily
// registers in our setup. Stub these too so the game's sceSifBindRpc
// succeeds for them and any subsequent CallRpc returns zeros instead of
// hanging.
//
// From scan of SLUS_213.76 ELF (0x8000xxxx range lui+ori patterns):
//   0x80000001 (FILEIO) - handled by p_black
//   0x80000004 (?)       - unidentified, 1 call site
//   0x80000007 (sceSio2man) - SIO2MAN.IRX preloaded, expect registered
//   0x80000008 (?)       - unidentified, 1 call site
//   0x80000009 (MC2?)    - MC2_D.IRX preloaded, expect registered
//   0x8000001a (?)       - unidentified, 1 call site
//   0x8000ffff           - wildcard/query, no need to register
//
// We register stubs for the unidentified ones. If a module also registers
// them later, IOP RPC system uses last-registered (typically wins), so
// real modules override our stubs - we're a safety net not a competitor.
#define STUB_SID_0x4   0x80000004
#define STUB_SID_0x8   0x80000008
#define STUB_SID_0x1a  0x8000001a
// agent-N22: RWA (RenderWare Audio) SIDs - extracted from RWA.IRX
#define STUB_SID_RWA_28  0x80000028
#define STUB_SID_RWA_30  0x80000030
#define STUB_SID_RWA_80  0x80000080
#define STUB_SID_RWA_AE  0x800000ae
#define STUB_SID_RWA_AF  0x800000af

// RPC server data and queues - one per SID since we need separate
// server descriptors. They share one DataQueue (one thread serves all).
static SifRpcServerData_t   sd_gtfs __attribute__((aligned(64)));
static SifRpcServerData_t   sd_4    __attribute__((aligned(64)));
static SifRpcServerData_t   sd_8    __attribute__((aligned(64)));
static SifRpcServerData_t   sd_1a   __attribute__((aligned(64)));
static SifRpcServerData_t   sd_rwa28 __attribute__((aligned(64)));
static SifRpcServerData_t   sd_rwa30 __attribute__((aligned(64)));
static SifRpcServerData_t   sd_rwa80 __attribute__((aligned(64)));
static SifRpcServerData_t   sd_rwaae __attribute__((aligned(64)));
static SifRpcServerData_t   sd_rwaaf __attribute__((aligned(64)));
static SifRpcDataQueue_t    dq __attribute__((aligned(64)));

// Reply buffer. The real GTFSCDVD's largest reply is ~32 bytes (a directory
// entry result struct). 256 bytes gives us comfortable headroom and matches
// typical SIF RPC buffer sizes. One per SID (separate so concurrent calls
// don't trample each other - but realistically only one server thread so
// they're serialized; still, separate buffers are safer).
static u8 rpcbuf_gtfs[256] __attribute__((aligned(64)));
static u8 rpcbuf_4   [256] __attribute__((aligned(64)));
static u8 rpcbuf_8   [256] __attribute__((aligned(64)));
static u8 rpcbuf_1a  [256] __attribute__((aligned(64)));
static u8 rpcbuf_rwa28[256] __attribute__((aligned(64)));
static u8 rpcbuf_rwa30[256] __attribute__((aligned(64)));
static u8 rpcbuf_rwa80[256] __attribute__((aligned(64)));
static u8 rpcbuf_rwaae[256] __attribute__((aligned(64)));
static u8 rpcbuf_rwaaf[256] __attribute__((aligned(64)));

// RPC server callback. Real GTFSCDVD callback dispatches on fno 1..6 and
// writes a single int result at *(int*)buf. We mimic that, but with
// per-fno return codes that hopefully push the game's caller into a
// "clean error" path rather than an "infinite retry" path:
//
//   fno 1 (TOC read):   return 0  ("0 entries read OK") - game might
//                       handle empty TOC gracefully
//   fno 2 (no-op):      return 0  (matches real behavior)
//   fno 3 (search):     return -1 ("file not found") - clean error
//   fno 4 (open):       return -1 ("can't open") - clean error
//   fno 5 (status):     return 0  ("idle, no data pending")
//   fno 6 (close):      return 0  ("closed OK")
//
// Pick whichever value most plausibly tells the game "I'm here, but no
// data for you" so it can exit cleanly with an error rather than spin
// forever waiting for an event that never fires.
static void *rpc_callback(int fno, void *buf, int size)
{
    // Zero the buffer first so out-of-spec fields are deterministic.
    int n = size > 64 ? 64 : size;
    int i;
    u8 *p = (u8 *)buf;
    for (i = 0; i < n; i++)
        p[i] = 0;

    // Write per-fno return code at offset 0 (the slot the real callback
    // uses for function result).
    int rc = 0;
    switch (fno) {
        case 1: rc =  0; break;  // TOC: 0 entries OK
        case 2: rc =  0; break;  // no-op
        case 3: rc = -1; break;  // search: not found
        case 4: rc = -1; break;  // open: error
        case 5: rc =  0; break;  // status: idle
        case 6: rc =  0; break;  // close: OK
        default: rc = -1; break; // unknown fno: error
    }
    *(int *)buf = rc;
    return buf;
}

// Generic stub callback for the unknown SIDs (0x80000004, 8, 1a).
// Same behavior as rpc_callback but doesn't have the per-fno switch -
// just zero everything and return.
static void *rpc_stub_callback(int fno, void *buf, int size)
{
    int n = size > 64 ? 64 : size;
    int i;
    u8 *p = (u8 *)buf;
    for (i = 0; i < n; i++)
        p[i] = 0;
    return buf;
}

// RPC server thread. SifSetRpcQueue/RegisterRpc/RpcLoop is the standard
// IOP RPC server pattern - see e.g. neutrino's cdvdfsv.c.
//
// We register 4 RPC servers sharing the same DataQueue (one thread serves
// all). Any of them that's also registered by a real IOP module later
// will be transparently overridden by the real one - we're a safety net.
static void rpc_server_thread(void *arg)
{
    sceSifSetRpcQueue(&dq, GetThreadId());
    sceSifRegisterRpc(&sd_gtfs, GTFSCDVD_SID, &rpc_callback,      rpcbuf_gtfs, NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_4,    STUB_SID_0x4, &rpc_stub_callback, rpcbuf_4,    NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_8,    STUB_SID_0x8, &rpc_stub_callback, rpcbuf_8,    NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_1a,   STUB_SID_0x1a,&rpc_stub_callback, rpcbuf_1a,   NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_rwa28, STUB_SID_RWA_28, &rpc_stub_callback, rpcbuf_rwa28, NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_rwa30, STUB_SID_RWA_30, &rpc_stub_callback, rpcbuf_rwa30, NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_rwa80, STUB_SID_RWA_80, &rpc_stub_callback, rpcbuf_rwa80, NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_rwaae, STUB_SID_RWA_AE, &rpc_stub_callback, rpcbuf_rwaae, NULL, NULL, &dq);
    sceSifRegisterRpc(&sd_rwaaf, STUB_SID_RWA_AF, &rpc_stub_callback, rpcbuf_rwaaf, NULL, NULL, &dq);
    sceSifRpcLoop(&dq);
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
