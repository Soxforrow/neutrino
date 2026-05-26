// libc/newlib
#include <string.h>

// PS2SDK
#include <iopcontrol.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <syscallnr.h>

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
            // agent-N5: do NOT BGERROR here. The original check halts execution
            // if the EE memory containing IOP modules has been modified since
            // the loader captured the checksum. For games that use the full
            // 32 MiB of EE RAM (Black/Criterion engine), even a high mod_base
            // (0x01f80000) doesn't keep this region pristine - the game's heap
            // or transient allocations reach into the checksum window.
            // The IOP modules are re-DMA'd from EE memory on every IOP reset
            // via SifExecModuleBuffer regardless, so a checksum mismatch just
            // means "the memory got touched", not "the modules are broken".
            // Continue and let the reset proceed.
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

    // agent-N10: distinct-color stage markers throughout New_Reset_Iop.
    // PPRINTF via _print doesn't reach ps2client (ps2link doesn't forward
    // SIO output), so use TV background color as the diagnostic channel.
    // Whatever color the TV settles on tells us exactly which step hung.
    //
    // STAGE COLORS:
    //   MAGENTA (255,0,255) - caller set, before entering New_Reset_Iop
    //   ORANGE  (255,128,0) - after SifAllocIopHeap + CopyToIop + imgdrv exec
    //   DK_BLUE (0,0,128)   - after _SifExecModuleBuffer/LoadModule for udnl
    //   LIME    (128,255,0) - after SIF reg writes (entering SifIopSync wait)
    //   TEAL    (0,128,128) - SifIopSync timed out (N7/N8)
    //   WHITE   (255,255,255) - after services_start
    //   GREEN   (0,255,0)   - after sbv_patch + module loop (all done)
    //   YELLOW  (255,255,0) - caller sets (reboot3 stage)
    //   BLACK   (0,0,0)     - services_exit done (caller)

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

    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = GSCOLOR32(255, 128, 0); // ORANGE = imgdrv loaded

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

    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = GSCOLOR32(0, 0, 128); // DK_BLUE = udnl loaded

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    Old_SifSetReg(SIF_SYSREG_RPCINIT, 0);
    Old_SifSetReg(SIF_SYSREG_SUBADDR, (int)NULL);
    ee_kmode_exit();
    EIntr();

    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = GSCOLOR32(128, 255, 0); // LIME = SIF regs reset, entering SifIopSync

    _iop_reboot_count++; // increment reboot counter to allow RPC clients to detect unbinding!

    // agent-N7: V12 SifIopSync timeout.
    // On SCPH-70012 V12 (first slim) silicon, the *second* and later calls to
    // New_Reset_Iop sometimes hang in SifIopSync forever - the first call works
    // because V12's SIF silicon is in a fresh post-BOOTEND state, but subsequent
    // calls hit a timing edge case where the IOP never signals back. Without a
    // timeout we lock up with TV=MAGENTA. With one, we transition to TEAL after
    // a few seconds and let services_start() try to bind anyway - frequently the
    // IOP IS actually responsive by then, the sync register just never updated.
    {
        // agent-N8: 5-second-ish timeout (~290M iterations at ~5 cycles each
        // on the 294 MHz EE). Was previously 4M which is ~70 ms - imperceptible
        // and user couldn't tell whether the timeout fired. Make it a visible
        // duration so the TEAL transition is observable on TV.
        volatile u32 iter = 0;
        while (!SifIopSync()) {
            if (++iter > 0x10000000) {
                if (eec.flags & EECORE_FLAG_DBC)
                    *GS_REG_BGCOLOR = COLOR_TEAL;
                break;
            }
        }
    }

    services_start();
    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = COLOR_WHITE; // WHITE = services_start done

    // Patch the IOP to support LoadModuleBuffer
    sbv_patch_enable_lmb();

    DPRINTF("Loading extra IOP modules...\n");
    // Skip the first modules:
    // 0 = IOPRP.IMG
    // 1 = imgdrv.irx
    // 2 = udnl.irx
    // agent-N12: per-module color markers in the SifExecModuleBuffer loop.
    // The N10 stage markers showed TV=WHITE, which sits between the
    // services_start() marker (WHITE) and the post-loop GREEN. That tells
    // us the hang is somewhere INSIDE this loop. Add a unique color for
    // each module slot index so we see exactly which module's SifExec
    // call is hanging.
    //
    // Slot-to-color mapping (cycles through bright distinguishable hues):
    //   slot 3  -> RED         (255,  0,  0)
    //   slot 4  -> ORANGE      (255,128,  0)
    //   slot 5  -> YELLOW      (255,255,  0) - same as reboot3 marker, careful
    //   slot 6  -> LIME        (128,255,  0)
    //   slot 7  -> GREEN       (  0,255,  0)
    //   slot 8  -> CYAN        (  0,255,255)
    //   slot 9  -> AZURE       (  0,128,255)
    //   slot 10 -> BLUE        (  0,  0,255)
    //   slot 11 -> VIOLET      (128,  0,255)
    //   slot 12 -> MAGENTA     (255,  0,255) - matches reboot2 marker, careful
    //   slot 13 -> PINK        (255,  0,128)
    //   slot 14 -> WHITE       (255,255,255) - matches services_start marker
    //   slot 15+ -> GRAY       (128,128,128)
    //
    // Each color is set BEFORE the corresponding SifExecModuleBuffer call.
    // If TV settles on one of these, that's the slot index that's hung.
    // agent-N13: extend per-module palette to cover slots 15-18 distinctly
    // so we can tell exactly which of LIBSD/MC2_D/RWA/GTFSCDVD is hanging.
    // (N12 used a generic GRAY for slot >= 15; user saw GRAY so we know
    // the hang is in one of those four).
    // agent-N20: bring back first_call guard. Modules persist across IOP
    // resets (UDNL doesn't clear pre-loaded module RAM). Second SifExec of
    // the same module hangs LOADCORE waiting for existing RPC bindings to
    // release. Only run the load loop the first time.
    static int first_module_load = 1;
    if (!first_module_load) {
        if (eec.flags & EECORE_FLAG_DBC)
            *GS_REG_BGCOLOR = COLOR_GREEN;
        DPRINTF("Skipping module reload (already loaded)\n");
        return;
    }
    first_module_load = 0;

    for (i = 3; i < irxtable->count; i++) {
        if (eec.flags & EECORE_FLAG_DBC) {
            u32 colors_per_slot[] = {
                GSCOLOR32(255,   0,   0),  // 3  RED      fhi_bd
                GSCOLOR32(255, 128,   0),  // 4  ORANGE   dev9_hidden
                GSCOLOR32(255, 255,   0),  // 5  YELLOW   smap
                GSCOLOR32(128, 255,   0),  // 6  LIME     ministack
                GSCOLOR32(  0, 255,   0),  // 7  GREEN    udpfs_bd
                GSCOLOR32(  0, 255, 255),  // 8  CYAN     fakemod
                GSCOLOR32(  0, 128, 255),  // 9  AZURE    p_black
                GSCOLOR32(  0,   0, 255),  // 10 BLUE     SIO2MAN
                GSCOLOR32(128,   0, 255),  // 11 VIOLET   SIO2D
                GSCOLOR32(255,   0, 255),  // 12 MAGENTA  DBCMAN
                GSCOLOR32(255,   0, 128),  // 13 PINK     DS2O
                GSCOLOR32(255, 255, 255),  // 14 WHITE    DSPROUTE
                GSCOLOR32(139,  69,  19),  // 15 BROWN    LIBSD
                GSCOLOR32(139,   0,   0),  // 16 DK_RED   MC2_D
                GSCOLOR32(  0,   0, 139),  // 17 DK_BLUE  RWA
                GSCOLOR32(  0, 100,   0),  // 18 DK_GREEN GTFSCDVD
            };
            int idx = i - 3;
            if (idx < (int)(sizeof(colors_per_slot)/sizeof(colors_per_slot[0])))
                *GS_REG_BGCOLOR = colors_per_slot[idx];
            else
                *GS_REG_BGCOLOR = GSCOLOR32(192, 192, 192); // light gray for slot 19+
        }
        irxptr_t p = irxtable->modules[i];
        SifExecModuleBuffer((void *)p.ptr, p.size, p.arg_len, p.args, NULL);
    }
    if (eec.flags & EECORE_FLAG_DBC)
        *GS_REG_BGCOLOR = GSCOLOR32(64, 64, 64); // DARK_GRAY = entire loop done

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

    if ((reboot1 + reboot2 + reboot3) == 0) {
        // agent-N26: even when we ignore the reset (iop_rm[X]=0 etc.),
        // bump _iop_reboot_count so RPC clients still re-bind. Without this,
        // game's sceSifSyncIop polls the unchanged counter and waits forever.
        _iop_reboot_count++;
        return;
    }

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
        // agent-N7: SifIopSync timeout, same rationale as in New_Reset_Iop above.
        {
            volatile u32 iter = 0;
            while (!SifIopSync()) {
                if (++iter > 0x00400000) {
                    if (eec.flags & EECORE_FLAG_DBC)
                        *GS_REG_BGCOLOR = COLOR_TEAL;
                    break;
                }
            }
        }
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
        // agent-N28: V12 silicon needs settling time before reboot3's
        // New_Reset_Iop. ALWAYS delay (not just after reboot2) - the IOP
        // is in a transient state from the game's just-arrived reset
        // request even when we don't fire reboot2 explicitly.
        // ~200ms at 294MHz EE.
        {
            volatile u32 spin = 0;
            while (spin < 0x02000000) { spin++; }
        }
        // V12 FIX: ignore game's IOPRP args, use neutrino's IOPRP only
        (void)arg; (void)arglen;
        New_Reset_Iop(NULL, 0);
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
    }
    // agent-N5: tolerate unexpected SetReg calls during the post-reset
    // state-machine window. Black/Criterion-engine titles do these in a
    // different order than the standard SDK, and halting via BGERROR
    // (previously here) prevented the game from booting at all. Just pass
    // through to Old_SifSetReg so the hardware actually gets the write.

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
    }
    // agent-N5: tolerate unexpected GetReg calls during the post-reset
    // state-machine window. Black/Criterion-engine titles query registers
    // in a different order than the standard SDK; halting via BGERROR
    // (previously here, producing the SOLID GREEN halt) prevented the
    // game from advancing. Pass through to Old_SifGetReg so the caller
    // gets a real register value.

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

    // agent-N24: V12 silicon may execute stale i-cache for the kernel
    // syscall vector after SetSyscall writes. Flush dcache so our writes
    // commit, then invalidate icache so CPU re-reads fresh kernel code.
    // Without this, the first call to a hooked syscall may read the OLD
    // (un-hooked) entry from icache.
    FlushCache(WRITEBACK_DCACHE);
    FlushCache(INVALIDATE_ICACHE);
}
