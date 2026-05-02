#include <loadcore.h>
#include <sifcmd.h>
#include <tamtypes.h>

#include "mprintf.h"
#include "ioplib.h"

#define MODNAME "p_black"
IRX_ID(MODNAME, 1, 1);

// The two SIF RPC server-IDs Black uses:
//   0x80000001 = FILEIO
//   0x80000002 = IOPMEM (Sony's IOP heap RPC)
//
// Black calls these servers with extended function numbers that
// Sony's stock fileio/iopmem modules don't handle:
//   - FILEIO: extended fno 0xff
//   - IOPMEM: extended fno 4
//
// Sony's stock callbacks reject those fnos which causes the game
// to hang. We wrap the registered callback so that "unknown"
// fno values silently return a small zeroed success-buffer.

#define SID_FILEIO 0x80000001
#define SID_IOPMEM 0x80000002

// Highest fno that Sony's stock fileio/iopmem handlers actually implement.
// Anything strictly above this threshold is considered an "extended" code
// from Black and is intercepted/stubbed. Sony's stock fileio uses
// fno 0..6 (open, close, read, write, lseek, ioctl, remove + a few more
// added in later revisions, but always well under 0x10). IOPMEM uses
// fno 0..2.
#define FILEIO_MAX_FNO 0x10
#define IOPMEM_MAX_FNO 0x03

// SIF callback function pointer signature (matches sceSifRegisterRpc)
typedef void *(*sif_rpc_fn_t)(int fno, void *buf, int size);

// Original sceSifRegisterRpc
typedef int (*fp_sceSifRegisterRpc)(SifRpcServerData_t *sd,
                                    int sid,
                                    sif_rpc_fn_t func,
                                    void *buf,
                                    sif_rpc_fn_t cfunc,
                                    void *cbuf,
                                    SifRpcDataQueue_t *qd);
static fp_sceSifRegisterRpc org_sceSifRegisterRpc;

// Per-server state (we only need to remember the original callback)
struct intercept_state {
    int sid;
    int max_fno;
    sif_rpc_fn_t org_func;
};

static struct intercept_state fileio_state = {SID_FILEIO, FILEIO_MAX_FNO, NULL};
static struct intercept_state iopmem_state = {SID_IOPMEM, IOPMEM_MAX_FNO, NULL};

// Shared dummy buffer that wrapped callbacks return for unknown fnos.
// 64 bytes of zeros is enough to satisfy any reasonable response size.
static u8 dummy_response[64] __attribute__((aligned(16)));

// Wrapper that gets registered in place of the real callback
static void *wrap_fileio(int fno, void *buf, int size)
{
    M_DEBUG("FILEIO rpc fno=0x%x size=%d\n", fno, size);

    if (fno > fileio_state.max_fno) {
        // Black extended code: stub success
        M_DEBUG("- stubbing extended FILEIO fno=0x%x\n", fno);
        // Zero a small chunk of the caller's buffer to signal success.
        if (buf != NULL && size > 0) {
            int n = size > 64 ? 64 : size;
            int i;
            u8 *b = (u8 *)buf;
            for (i = 0; i < n; i++)
                b[i] = 0;
            return buf;
        }
        return dummy_response;
    }

    if (fileio_state.org_func != NULL)
        return fileio_state.org_func(fno, buf, size);

    return dummy_response;
}

static void *wrap_iopmem(int fno, void *buf, int size)
{
    M_DEBUG("IOPMEM rpc fno=0x%x size=%d\n", fno, size);

    if (fno > iopmem_state.max_fno) {
        // Black extended code (e.g. 4): stub success
        M_DEBUG("- stubbing extended IOPMEM fno=0x%x\n", fno);
        if (buf != NULL && size > 0) {
            int n = size > 64 ? 64 : size;
            int i;
            u8 *b = (u8 *)buf;
            for (i = 0; i < n; i++)
                b[i] = 0;
            return buf;
        }
        return dummy_response;
    }

    if (iopmem_state.org_func != NULL)
        return iopmem_state.org_func(fno, buf, size);

    return dummy_response;
}

// Hooked sceSifRegisterRpc: detect FILEIO/IOPMEM registrations and
// wrap their callbacks so unknown fnos get stubbed.
static int hooked_sceSifRegisterRpc(SifRpcServerData_t *sd,
                                    int sid,
                                    sif_rpc_fn_t func,
                                    void *buf,
                                    sif_rpc_fn_t cfunc,
                                    void *cbuf,
                                    SifRpcDataQueue_t *qd)
{
    sif_rpc_fn_t use_func = func;

    M_DEBUG("sceSifRegisterRpc(sid=0x%x, func=0x%x)\n", sid, (unsigned int)func);

    if (sid == SID_FILEIO) {
        M_DEBUG("- intercepting FILEIO server 0x%x\n", sid);
        fileio_state.org_func = func;
        use_func = wrap_fileio;
    } else if (sid == SID_IOPMEM) {
        M_DEBUG("- intercepting IOPMEM server 0x%x\n", sid);
        iopmem_state.org_func = func;
        use_func = wrap_iopmem;
    }

    return org_sceSifRegisterRpc(sd, sid, use_func, buf, cfunc, cbuf, qd);
}

int _start(int argc, char **argv)
{
    M_DEBUG("p_black: hooking sifcmd::sceSifRegisterRpc\n");

    // Look up sifcmd. sceSifRegisterRpc is export #20 in standard sifcmd.
    iop_library_t *lib_sifcmd = ioplib_getByName("sifcmd\0\0");
    if (lib_sifcmd == NULL) {
        M_DEBUG("- sifcmd library not found, aborting\n");
        return MODULE_NO_RESIDENT_END;
    }

    // sceSifRegisterRpc is at fixed export index 20 in sifcmd.
    org_sceSifRegisterRpc = ioplib_hookExportEntry(lib_sifcmd, 20, hooked_sceSifRegisterRpc);
    if (org_sceSifRegisterRpc == NULL) {
        M_DEBUG("- failed to hook sceSifRegisterRpc\n");
        return MODULE_NO_RESIDENT_END;
    }

    ioplib_relinkExports(lib_sifcmd);

    M_DEBUG("p_black: hook installed\n");
    return MODULE_RESIDENT_END;
}
