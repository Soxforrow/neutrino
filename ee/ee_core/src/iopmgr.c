// libc/newlib
#include <string.h>

// PS2SDK
#include <iopcontrol.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <syscallnr.h>
#include <libcdvd.h>

// Neutrino
#include "ee_debug.h"
#include "iopmgr.h"
#include "asm.h"
#include "util.h"
#include "eecore_config.h"

extern int _iop_reboot_count; // defined in libkernel (iopcontrol.c)

static int set_reg_hook = 0;
static int get_reg_hook = 0;
static int imgdrv_offset = 0;
static void (*Direct_SetSyscall)(s32 syscall_num, void *handler);
static int (*Old_SifSetReg)(u32 register_num, int register_value);
static int (*Old_SifGetReg)(u32 register_num);

// Used by Hook_SifSetDma in asm.S
u32 (*Old_SifSetDma)(SifDmaTransfer_t *sdd, s32 len);

int _SifExecModuleBuffer(const void *ptr, u32 size, u32 arg_len, const char *args, int *mod_res, int dontwait);
int _SifLoadModule(const char *path, int arg_len, const char *args, int *modres, int fno, int dontwait);

//---------------------------------------------------------------------------
// agent-K2: Preload a fixed list of game-specific IOP modules from disc.
//
// Background
// ----------
// When the V12 fix uses neutrino's IOPRP instead of the game's IOPRP, the
// custom IOP modules embedded in the game's IOPRPxxx.IMG never get loaded by
// UDNL. Black (SLUS_213.76) hangs at sector ~7609000 because GTFSCDVD/RWA/MC2_D
// and friends are missing — the game DMAs RPC packets to servers that don't
// exist and waits forever.
//
// Implementation
// --------------
// We assume neutrino's IOPRP has loaded cdvdfsv (which it does), so we can use
// the IOP-side LOADFILE service to read each .IRX from the game's
// 'cdrom0:\IOP' directory. Many games keep loose .IRX copies in that folder
// for use by LOADFILE; the previous agent-K confirmed this is the right path.
//
// We emit always-on prints (PPRINTF -> _print, requires -ldebug) so the disc
// I/O activity is visible in ps2client output even on production builds.
//
// Skip-list of stock IOP modules already loaded by neutrino's IOPRP. Re-loading
// these would conflict with the running instances.
//---------------------------------------------------------------------------
static int iopmgr_is_stock_module(const char *name)
{
    static const char * const skip_list[] = {
        "RESET",   "ROMDIR",  "EXTINFO", "SYSMEM",   "LOADCORE",
        "SIFCMD",  "SIFMAN",  "THREADMAN","IOMAN",   "MODLOAD",
        "FILEIO",  "CDVDMAN", "CDVDFSV", "LOADFILE", "TIMEMANI",
        "ROMDRV",  "EESYNC",  "SYSCLIB", "STDIO",    NULL,
    };
    int i;
    for (i = 0; skip_list[i] != NULL; i++) {
        const char *a = skip_list[i];
        const char *b = name;
        while (*a && *b) {
            char ca = (*a >= 'a' && *a <= 'z') ? (*a - 'a' + 'A') : *a;
            char cb = (*b >= 'a' && *b <= 'z') ? (*b - 'a' + 'A') : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == '\0' && (*b == '\0' || *b == '.'))
            return 1;
    }
    return 0;
}

int iopmgr_preload_game_modules(void)
{
    // Game-specific IOP modules expected on cdrom0 IOP directory.
    // List covers Black (SLUS_213.76) and similar Sony first-party titles.
    static const char * const game_irx_list[] = {
        // Generic Sony I/O backbone modules (loaded first)
        "cdrom0:\\IOP\\SIO2MAN.IRX;1",
        "cdrom0:\\IOP\\PADMAN.IRX;1",
        "cdrom0:\\IOP\\MCMAN.IRX;1",
        "cdrom0:\\IOP\\MCSERV.IRX;1",
        "cdrom0:\\IOP\\LIBSD.IRX;1",
        "cdrom0:\\IOP\\SDRDRV.IRX;1",
        // Black-specific I/O modules
        "cdrom0:\\IOP\\SIO2D.IRX;1",
        "cdrom0:\\IOP\\DBCMAN.IRX;1",
        "cdrom0:\\IOP\\DS2O.IRX;1",
        "cdrom0:\\IOP\\DSPROUTE.IRX;1",
        // Black-specific subsystem modules
        "cdrom0:\\IOP\\MC2_D.IRX;1",
        "cdrom0:\\IOP\\RWA.IRX;1",
        "cdrom0:\\IOP\\GTFSCDVD.IRX;1",
        NULL,
    };
    int idx;
    int loaded = 0;
    int failed = 0;
    int rc;
    int i;
    volatile int j;

    // _print needs InitDebug() to enable SIO output. It's idempotent, so calling
    // it every time we run is safe.
    PPINIT();

    PPRINTF("preload: begin (services_start already called by New_Reset_Iop)\n");

    // agent-K3: Initialize EE-side CDVD layer. SCECdINoD = no disc detect (we
    // assume disc is already present since we just booted from it). This
    // primes the EE-side cdvd helper state so subsequent sceCdSync() polls
    // make sense.
    PPRINTF("preload: sceCdInit(SCECdINoD)...\n");
    sceCdInit(SCECdINoD);
    PPRINTF("preload: sceCdInit done\n");

    // agent-K3: Wait for IOP to finish syncing post-reboot. SifIopSync()
    // returns non-zero when the IOP is ready to accept SIF traffic. This
    // protects against a race where ee_core's preloader tries to talk to
    // cdvdfsv before the IOP-side RPC server has actually bound.
    PPRINTF("preload: waiting for SifIopSync...\n");
    for (i = 0; i < 100; i++) {
        if (SifIopSync()) {
            PPRINTF("preload: SifIopSync OK (iter=%d)\n", i);
            break;
        }
        // small delay between sync polls
        for (j = 0; j < 100000; j++) ;
    }
    if (i == 100)
        PPRINTF("preload: SifIopSync TIMEOUT after 100 iters - continuing anyway\n");

    // agent-K3: Drain any pending CDVD I/O that may be lingering from boot.
    PPRINTF("preload: sceCdSync(0)...\n");
    sceCdSync(0);
    PPRINTF("preload: sceCdSync done\n");

    // agent-K3: Explicit grace delay so IOP modules (cdvdfsv, LOADFILE, etc.)
    // have time to finish binding their RPC servers. The IOP boot sequence
    // is asynchronous: SifIopSync only confirms the kernel is up, not that
    // every module has finished its init thread.
    PPRINTF("preload: grace delay (10M iters)...\n");
    for (j = 0; j < 10000000; j++) ;
    PPRINTF("preload: grace delay done\n");

    // Make sure LOADFILE RPC is bound. SifLoadFileInit() is called from
    // services_start(), but it's idempotent — calling again is safe and gives
    // us an early-failure signal if LOADFILE isn't actually up.
    PPRINTF("preload: SifLoadFileInit...\n");
    rc = SifLoadFileInit();
    if (rc < 0) {
        PPRINTF("preload: SifLoadFileInit failed rc=%d - aborting\n", rc);
        return 0;
    }
    PPRINTF("preload: SifLoadFileInit OK\n");

    for (idx = 0; game_irx_list[idx] != NULL; idx++) {
        const char *path = game_irx_list[idx];
        int retries;

        // Extract bare module name for skip-list check.
        const char *bare = path;
        const char *p;
        for (p = path; *p; p++) {
            if (*p == '\\' || *p == '/' || *p == ':')
                bare = p + 1;
        }
        if (iopmgr_is_stock_module(bare)) {
            PPRINTF("preload: skip stock %s\n", bare);
            continue;
        }

        // agent-K3: retry up to 3 times if the load fails. The first read
        // after IOP reset is most prone to timeout, so subsequent retries
        // tend to succeed once cdvdfsv warms up.
        rc = -1;
        for (retries = 0; retries < 3; retries++) {
            PPRINTF("preload: trying %s (attempt %d/3)\n", path, retries + 1);

            // Synchronous load (dontwait=0) so each module finishes before next.
            // LF_F_MOD_LOAD = standard module load by path.
            rc = _SifLoadModule(path, 0, NULL, NULL, LF_F_MOD_LOAD, 0);
            if (rc >= 0)
                break;

            PPRINTF("preload: attempt %d failed rc=%d, retrying\n", retries + 1, rc);
            // wait between retries (~5M iters)
            for (j = 0; j < 5000000; j++) ;
        }

        if (rc < 0) {
            PPRINTF("preload: FAILED %s rc=%d (after retries)\n", path, rc);
            failed++;
            // Tolerate failures - missing file or load error.
        } else {
            PPRINTF("preload: OK %s id=%d\n", path, rc);
            loaded++;
        }
    }

    PPRINTF("preload: done loaded=%d failed=%d\n", loaded, failed);
    return loaded;
}

//---------------------------------------------------------------------------
void services_start()
{
    DPRINTF("Starting services...\n");
    SifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
}

//---------------------------------------------------------------------------
void services_exit()
{
    DPRINTF("Exiting services...\n");
    SifExitIopHeap();
    SifLoadFileExit();
    SifExitRpc();
}

//---------------------------------------------------------------------------
// Simple module storage checksum
static void module_checksum()
{
    int i, j;
    u32 *pms = (u32 *)eec.ModStorageStart;

    DPRINTF("Module memory checksum:\n");

    for (j = 0; j < EEC_MOD_CHECKSUM_COUNT; j++) {
        u32 ssv = 0;
        for (i=0; i<1024; i++) {
            ssv += pms[i];
            // Skip imgdrv patch area
            if (pms[i] == 0xDEC1DEC1)
                i += 2;
        }
        if (ssv == eec.mod_checksum_4k[j]) {
            DPRINTF("- 0x%08x = 0x%08x\n", (u32)pms, ssv);
        } else {
            DPRINTF("- 0x%08x = 0x%08x != 0x%08x\n", (u32)pms, ssv, eec.mod_checksum_4k[j]);
            BGERROR(COLOR_FUNC_IOPREBOOT, 2);
        }
        pms += 1024;
    }
}

#ifdef __EESIO_DEBUG
//---------------------------------------------------------------------------
static void print_iop_args(int arg_len, const char *args)
{
    // Multiple null terminated strings together
    int args_idx = 0;
    int was_null = 1;

    if (arg_len == 0)
        return;

    DPRINTF("IOP reboot arguments (arg_len=%d):\n", arg_len);

    // Search strings
    while(args_idx < arg_len) {
        if (args[args_idx] == 0) {
            if (was_null == 1) {
                DPRINTF("- args[%d]=0\n", args_idx);
            }
            was_null = 1;
        }
        else if (was_null == 1) {
            DPRINTF("- args[%d]='%s'\n", args_idx, &args[args_idx]);
            was_null = 0;
        }
        args_idx++;
    }
}
#endif

//---------------------------------------------------------------------------
// Reset IOP. This function replaces SifIopReset from the PS2SDK
static int Reset_Iop(const char *arg, int mode)
{
    static SifCmdResetData_t reset_pkt __attribute__((aligned(64)));
    struct t_SifDmaTransfer dmat;
    int arglen;

    _iop_reboot_count++; // increment reboot counter to allow RPC clients to detect unbinding!

    SifStopDma();

    for (arglen = 0; arg[arglen] != '\0'; arglen++)
        reset_pkt.arg[arglen] = arg[arglen];

    reset_pkt.header.psize = sizeof reset_pkt; // dsize is not initialized (and not processed, even on the IOP).
    reset_pkt.header.cid = SIF_CMD_RESET_CMD;
    reset_pkt.arglen = arglen;
    reset_pkt.mode = mode;

    dmat.src = &reset_pkt;
    dmat.dest = (void *)SifGetReg(SIF_SYSREG_SUBADDR);
    dmat.size = sizeof(reset_pkt);
    dmat.attr = SIF_DMA_ERT | SIF_DMA_INT_O;
    SifWriteBackDCache(&reset_pkt, sizeof(reset_pkt));

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);

    if (!Old_SifSetDma(&dmat, 1)) {
        ee_kmode_exit();
        EIntr();
        return 0;
    }

    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    Old_SifSetReg(SIF_SYSREG_RPCINIT, 0);
    Old_SifSetReg(SIF_SYSREG_SUBADDR, (int)NULL);
    ee_kmode_exit();
    EIntr();

    return 1;
}

//---------------------------------------------------------------------------
// Reset IOP to include our modules
void New_Reset_Iop(const char *arg, int arglen)
{
    int i;
    void *pIOP_buffer;
    const void *IOPRP_img, *imgdrv_irx, *udnl_irx;
    unsigned int length_rounded, udnl_cmdlen, size_IOPRP_img, size_imgdrv_irx, size_udnl_irx;
    char udnl_mod[10];
    char udnl_cmd[RESET_ARG_MAX + 1];
    irxtab_t *irxtable = (irxtab_t *)eec.ModStorageStart;

    DPRINTF("%s()\n", __FUNCTION__);
#ifdef __EESIO_DEBUG
    print_iop_args(arglen, arg);
#endif

    udnl_cmdlen = 0;
    if (arglen >= 10) {
        // Copy: rom0:UDNL or rom1:UDNL
        // - Are these the only update modules? Always 9 chars long?
        strncpy(udnl_mod, &arg[0], 10);
        // Make sure it's 0 terminated
        udnl_mod[10-1] = '\0';

        if (arglen > 10) {
            // Copy: arguments
            udnl_cmdlen = arglen-10; // length, including terminating 0
            strncpy(udnl_cmd, &arg[10], udnl_cmdlen);

            // Fix if 0 is not included in length
            if (udnl_cmd[udnl_cmdlen-1] != '\0') {
                udnl_cmd[udnl_cmdlen] = '\0';
                udnl_cmdlen++;
            }
        }
    } else {
        strncpy(udnl_mod, "rom0:UDNL", 10);
    }

    // Add our own IOPRP image
    strncpy(&udnl_cmd[udnl_cmdlen], "img0:", 6);
    udnl_cmdlen += 6;

    // FIXED modules:
    // 0 = IOPRP.img
    // 1 = imgdrv.irx
    // 2 = udnl.irx
    IOPRP_img       = irxtable->modules[0].ptr;
    size_IOPRP_img  = irxtable->modules[0].size;
    imgdrv_irx      = irxtable->modules[1].ptr;
    size_imgdrv_irx = irxtable->modules[1].size;
    udnl_irx        = irxtable->modules[2].ptr;
    size_udnl_irx   = irxtable->modules[2].size;

    // Manually copy IOPRP to IOP
    length_rounded = (size_IOPRP_img + 0xF) & ~0xF;
    pIOP_buffer = SifAllocIopHeap(length_rounded);
    CopyToIop(IOPRP_img, length_rounded, pIOP_buffer);

    // Patch imgdrv.irx to point to the IOPRP
    for (i = 0; i < size_imgdrv_irx; i += 4) {
        if (*(u32 *)((&((unsigned char *)imgdrv_irx)[i])) == 0xDEC1DEC1) {
            imgdrv_offset = i;
            break;
        }
    }
    *(void **)(UNCACHED_SEG(&((unsigned char *)imgdrv_irx)[imgdrv_offset+4])) = pIOP_buffer;
    *(u32   *)(UNCACHED_SEG(&((unsigned char *)imgdrv_irx)[imgdrv_offset+8])) = size_IOPRP_img;

    // Load patched imgdrv.irx
    SifExecModuleBuffer((void *)imgdrv_irx, size_imgdrv_irx, 0, NULL, NULL);

    // Trigger IOP reboot with update
    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    ee_kmode_exit();
    EIntr();

    if (udnl_irx != NULL) {
        // Load custom UDNL
        _SifExecModuleBuffer(udnl_irx, size_udnl_irx, udnl_cmdlen, udnl_cmd, NULL, 1);
    }
    else {
        // Load system UDNL
        _SifLoadModule(udnl_mod, udnl_cmdlen, udnl_cmd, NULL, LF_F_MOD_LOAD, 1);
    }

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    Old_SifSetReg(SIF_SYSREG_RPCINIT, 0);
    Old_SifSetReg(SIF_SYSREG_SUBADDR, (int)NULL);
    ee_kmode_exit();
    EIntr();

    _iop_reboot_count++; // increment reboot counter to allow RPC clients to detect unbinding!

    while (!SifIopSync()) {
        ;
    }

    services_start();
    // Patch the IOP to support LoadModuleBuffer
    sbv_patch_enable_lmb();

    DPRINTF("Loading extra IOP modules...\n");
    // Skip the first modules:
    // 0 = IOPRP.IMG
    // 1 = imgdrv.irx
    // 2 = udnl.irx
    for (i = 3; i < irxtable->count; i++) {
        irxptr_t p = irxtable->modules[i];
        SifExecModuleBuffer((void *)p.ptr, p.size, p.arg_len, p.args, NULL);
    }

    DPRINTF("New_Reset_Iop complete!\n");

    return;
}

void New_Reset_Iop2(const char *arg, int arglen, int eeload)
{
    static int iopstate = 0;
    static int resetcount = 0;
    static int eeload_prev = 0;

    int reboot1 = 0;
    int reboot2 = 0;
    int reboot3 = 0;

    DPRINTF("%s(..., %d, %d)\n", __FUNCTION__, arglen, eeload);

    if (eeload) {
        int reboot_mode = eec.iop_rm[0];
        if (reboot_mode == 1) {
            reboot2 = (resetcount == 0) ? 1 : 0;
        } else if (reboot_mode == 2) {
            reboot2 = 1;
        } else if (reboot_mode == 3) {
            reboot1 = 1;
            reboot2 = 1;
        }
    } else {
        int reboot_mode = eec.iop_rm[1];
        if (reboot_mode == 1) {
            reboot3 = 1;
        } else if (reboot_mode == 2) {
            reboot2 = (iopstate != 2) ? 1 : 0;
            reboot3 = 1;
        } else if (reboot_mode == 3) {
            reboot1 = (iopstate != 2) ? 1 : 0;
            reboot2 = (iopstate != 2) ? 1 : 0;
            reboot3 = 1;
        } else if (reboot_mode == 4) {
            reboot1 = 1;
            reboot2 = 1;
            reboot3 = 1;
        }
    }

    // Ignore duplicate IOP resets
    // Normally the reboots are:
    // - 1x on EE ELF load
    // - 1x for game IOPRP
    // This pattern repeats for every EE ELF loaded
    // However Max Payne reboots the IOP also BEFORE loading a new EE ELF
    // This is not needed and causes issues in OPL/neutrino
    if (eec.iop_rm[2] == 1 && eeload_prev == 0 && eeload == 0) {
        DPRINTF("- Ignore duplicate IOP resets\n");
        reboot1 = 0;
        reboot2 = 0;
        reboot3 = 0;
    }
    eeload_prev = eeload;

    DPRINTF("- performing reboots: %c-%c-%c\n", reboot1?'1':'X', reboot2?'2':'X', reboot3?'3':'X');

    if ((reboot1 + reboot2 + reboot3) == 0)
        return;

    // Validate module storage
    module_checksum();

    // Start services, some games hang here becouse the IOP is not responding
    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = COLOR_LBLUE;

    if (reboot1) {
        // Reboot the IOP to base state
        DPRINTF("%s: reboot1: IOP to base state\n", __FUNCTION__);
        SifInitRpc(0);
        while (!Reset_Iop("", 0)) {}
        while (!SifIopSync()) {}
        services_start();
        sbv_patch_enable_lmb();
        // Unusable state, no neutrino modules loaded
        iopstate = 1;
    } else {
        services_start();
    }

    if (reboot2) {
        // Reboot the IOP with neutrino modules
        DPRINTF("%s: reboot2: IOP with neutrino modules\n", __FUNCTION__);
        if (eec.flags & EECORE_FLAG_DBC)
            *GS_REG_BGCOLOR = COLOR_MAGENTA;
        New_Reset_Iop(NULL, 0);
        // Known clean neutrino reboot state
        iopstate = 2;
    }

    if (reboot3) {
        DPRINTF("%s: reboot3: IOP with neutrino modules and IOPRP (V12 fix: ignoring game args)\n", __FUNCTION__);
#ifdef __EESIO_DEBUG
        print_iop_args(arglen, arg);
#endif
        if (eec.flags & EECORE_FLAG_DBC)
            *GS_REG_BGCOLOR = COLOR_YELLOW;
        // V12 FIX: ignore game's IOPRP args, use neutrino's IOPRP only
        // The game's IOPRP triggers a hang on early V12 SCPH-7000x silicon
        // when its UDNL command sequence interacts with neutrino's modules.
        // Using neutrino's IOPRP (NULL, 0) keeps the IOP in a known good state.
        // Game's custom modules (GTFSCDVD, RWA, etc.) still load via subsequent
        // SifExecModuleBuffer calls.
        (void)arg; (void)arglen;
        New_Reset_Iop(NULL, 0);

        // agent-K2: now that neutrino's modules are running and cdvdfsv RPC is
        // bound by services_start() inside New_Reset_Iop, walk the game's
        // expected IOP module list on the IOP directory of the disc and load
        // each via LOADFILE. Without this, Black (SLUS_213.76) hangs because
        // GTFSCDVD/RWA/MC2_D never load after V12 fix swaps in neutrino IOPRP.
        if (eec.flags & EECORE_FLAG_DBC)
            *GS_REG_BGCOLOR = COLOR_ORANGE;
        iopmgr_preload_game_modules();
        if (eec.flags & EECORE_FLAG_DBC)
            *GS_REG_BGCOLOR = COLOR_GREEN;

        // The game will use the IOP for unknown purposes now
        iopstate = 3;
    }

    resetcount++;

    // Exit services
    services_exit();

    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = COLOR_BLACK;
}

//---------------------------------------------------------------------------
// This function is called when SifSetDma catches a reboot request
u32 New_SifSetDma(SifDmaTransfer_t *sdd, s32 len)
{
    struct _iop_reset_pkt *reset_pkt = (struct _iop_reset_pkt *)sdd->src;

    New_Reset_Iop2(reset_pkt->arg, reset_pkt->arglen, 0);

    // Ignore EE still trying to complete the IOP reset
    set_reg_hook = 4;
    get_reg_hook = 1;

    return 1;
}

//---------------------------------------------------------------------------
// Function running in kernel mode!
// No printf and keep as simple as possible!
static int Hook_SifSetReg(u32 register_num, int register_value)
{
    if (set_reg_hook == 4 && register_num == SIF_REG_SMFLAG && register_value == SIF_STAT_SIFINIT) {
        set_reg_hook--;
        return 0;
    } else if (set_reg_hook == 3 && register_num == SIF_REG_SMFLAG && register_value == SIF_STAT_CMDINIT) {
        set_reg_hook--;
        return 0;
    } else if (set_reg_hook == 2 && register_num == SIF_SYSREG_RPCINIT && register_value == 0) {
        set_reg_hook--;
        return 0;
    } else if (set_reg_hook == 1 && register_num == SIF_SYSREG_SUBADDR && register_value == (int)NULL) {
        set_reg_hook--;
        if (eec.flags & EECORE_FLAG_UNHOOK) {
            //
            // Call kernel functions directly, becouse we are already in kernel mode
            //
            Direct_SetSyscall(__NR_SifSetDma, Old_SifSetDma);
            Direct_SetSyscall(__NR_SifSetReg, Old_SifSetReg);
            Direct_SetSyscall(__NR_SifGetReg, Old_SifGetReg);
        }
        return 0;
    } else if (set_reg_hook == 0 && register_num == SIF_REG_SMFLAG && register_value == SIF_STAT_BOOTEND) {
        // Start of a new reboot sequence
        return 0;
    } else if (set_reg_hook != 0) {
        BGERROR(COLOR_FUNC_IOPREBOOT, 3);
    }

    return Old_SifSetReg(register_num, register_value);
}

//---------------------------------------------------------------------------
// Function running in kernel mode!
// No printf and keep as simple as possible!
static int Hook_SifGetReg(u32 register_num)
{
    if (get_reg_hook == 1 && register_num == SIF_REG_SMFLAG) {
        get_reg_hook--;
        return 0;
    } else if (get_reg_hook != 0) {
        BGERROR(COLOR_FUNC_IOPREBOOT, 4);
    }

    return Old_SifGetReg(register_num);
}

//---------------------------------------------------------------------------
// Replace SifSetDma, SifSetReg and SifGetReg syscalls in kernel
void Install_Kernel_Hooks(void)
{
    Direct_SetSyscall = GetSyscallHandler(__NR_SetSyscall);

    Old_SifSetDma = GetSyscallHandler(__NR_SifSetDma);
    SetSyscall(__NR_SifSetDma, &Hook_SifSetDma);

    Old_SifSetReg = GetSyscallHandler(__NR_SifSetReg);
    SetSyscall(__NR_SifSetReg, &Hook_SifSetReg);

    Old_SifGetReg = GetSyscallHandler(__NR_SifGetReg);
    SetSyscall(__NR_SifGetReg, &Hook_SifGetReg);
}
