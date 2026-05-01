#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <xmodload.h>

#include "fakemod_config.h"
#include "ioplib.h"
#include "elf.h"
#include "mprintf.h"

#define MODNAME "fakemod"
IRX_ID(MODNAME, 1, 1);

struct fakemod_data fmd = {MODULE_SETTINGS_MAGIC};

// MODLOAD's exports pointers
static int (*org_LoadStartModule)(char *modpath, int arg_len, char *args, int *modres);
static int (*org_StartModule)(int id, char *modname, int arg_len, char *args, int *modres);
static int (*org_LoadModuleBuffer)(void *ptr);
static int (*org_StopModule)(int id, int arg_len, char *args, int *modres);
static int (*org_UnloadModule)(int id);
static int (*org_SearchModuleByName)(const char *modname);
static int (*org_ReferModuleStatus)(int mid, ModuleStatus *status);

/* ------------------------------------------------------------------
 * RPC fallback shim (agent-C)
 *
 * Some games (e.g. Black SLUS_213.76) issue RPC calls to FILEIO_service
 * (sid 0x80000001) and IOP_SIF_rpc_interface / sce_iopmem (sid 0x80000003)
 * with codes that the standard Sony IOP modules in IOPRP do not implement.
 * The default behavior for those modules is to printf "unrecognized code N"
 * and return NULL, which causes the EE-side caller to wait forever for a
 * reply that never comes.
 *
 * This shim wraps any RPC server callback that fakemod can reach (either by
 * intercepting future sceSifRegisterRpc calls or by patching SifRpcServerData
 * structs of already-registered services). The wrapper invokes the original
 * handler; if the handler returns NULL (the standard "I don't understand
 * this code" path), it returns the input buffer instead so the RPC machinery
 * still sends a reply, allowing the game to make forward progress.
 *
 * NOTE: this is a best-effort, "make the game stop hanging" workaround. The
 * data the game sees in the reply is whatever was in the request buffer.
 * If a particular game depends on specific reply contents for an unknown
 * code, additional fno-specific synthesis may be required.
 * ------------------------------------------------------------------ */

/* Forward decl — provided by sifcmd if exported. We reach it via the export
 * table below rather than an import stub so that fakemod still links on
 * older sifcmd builds where the table layout differs. */
typedef struct sif_rpc_server_data {
    int sid;
    void *(*function)(int fno, void *buf, int size);
    void *buff;
    int size;
    void *cfunction;
    void *cbuff;
    int csize;
    void *queue;          /* SifRpcDataQueue_t * */
    struct sif_rpc_server_data *link;
    struct sif_rpc_server_data *next;
    void *client;         /* SifRpcClientData_t * */
    int  fno;
    int  rsize;
    int  rmode;
    int  rid;
    void *receive;
    int  rcount;
    void *paddr;
    int  pdata;
} sif_rpc_server_data_t;

#define RPC_SHIM_MAX 16
struct rpc_shim_entry {
    int sid;
    void *(*orig)(int fno, void *buf, int size);
};
static struct rpc_shim_entry rpc_shim_table[RPC_SHIM_MAX];
static int rpc_shim_count = 0;

/* sceSifRegisterRpc original pointer (filled in _start). */
static void (*org_sceSifRegisterRpc)(void *sd, int sid, void *func, void *buff,
                                     void *cfunc, void *cbuff, void *queue);

static struct rpc_shim_entry *find_shim(int sid)
{
    int i;
    for (i = 0; i < rpc_shim_count; i++) {
        if (rpc_shim_table[i].sid == sid)
            return &rpc_shim_table[i];
    }
    return NULL;
}

static void *rpc_shim_dispatch(int sid, int fno, void *buf, int size)
{
    struct rpc_shim_entry *e = find_shim(sid);
    void *rv = NULL;

    if (e != NULL && e->orig != NULL)
        rv = e->orig(fno, buf, size);

    if (rv == NULL) {
        M_DEBUG("rpc_shim: sid=0x%x fno=0x%x returned NULL, faking success\n", sid, fno);
        rv = buf;
    }

    return rv;
}

/* Per-sid trampolines so the wrapper knows which sid it's serving without
 * relying on sifcmd internals. We pre-allocate one for each sid we care
 * about; this matches the small fixed set we want to patch. */
static void *shim_thunk_fileio(int fno, void *buf, int size)
{
    return rpc_shim_dispatch(0x80000001, fno, buf, size);
}

static void *shim_thunk_iopmem(int fno, void *buf, int size)
{
    return rpc_shim_dispatch(0x80000003, fno, buf, size);
}

static void *shim_thunk_for_sid(int sid)
{
    switch (sid) {
        case 0x80000001: return (void *)&shim_thunk_fileio;
        case 0x80000003: return (void *)&shim_thunk_iopmem;
        default: return NULL;
    }
}

/* Hook: if a service we care about registers, intercept it. */
static void Hook_sceSifRegisterRpc(void *sd, int sid, void *func, void *buff,
                                   void *cfunc, void *cbuff, void *queue)
{
    void *thunk = shim_thunk_for_sid(sid);
    if (thunk != NULL && rpc_shim_count < RPC_SHIM_MAX) {
        M_DEBUG("Hook_sceSifRegisterRpc: wrapping sid=0x%x func=%p\n", sid, func);
        rpc_shim_table[rpc_shim_count].sid = sid;
        rpc_shim_table[rpc_shim_count].orig = (void *(*)(int, void *, int))func;
        rpc_shim_count++;
        func = thunk;
    }
    org_sceSifRegisterRpc(sd, sid, func, buff, cfunc, cbuff, queue);
}

/* Best-effort patch of an already-registered server data. We walk the
 * sifcmd library's exports for sceSifGetServerData (export #16 in modern
 * sifcmd) and use it to find the SifRpcServerData_t for the requested sid,
 * then swap in our wrapper. */
typedef sif_rpc_server_data_t *(*sce_get_server_data_t)(int sid);

static int patch_existing_rpc(iop_library_t *lib_sifcmd, int sid)
{
    sce_get_server_data_t get_sd;
    sif_rpc_server_data_t *sd;
    void *thunk = shim_thunk_for_sid(sid);

    if (lib_sifcmd == NULL || thunk == NULL)
        return -1;

    /* sceSifGetServerData is export 16 in PS2SDK sifcmd (0x102+). It
     * iterates the internal queue list and returns the matching server
     * data, or NULL if no service is registered for sid. */
    if (ioplib_getTableSize(lib_sifcmd) <= 16)
        return -1;

    get_sd = (sce_get_server_data_t)lib_sifcmd->exports[16];
    if (get_sd == NULL)
        return -1;

    sd = get_sd(sid);
    if (sd == NULL) {
        M_DEBUG("patch_existing_rpc: sid=0x%x not registered yet\n", sid);
        return -1;
    }

    if (rpc_shim_count >= RPC_SHIM_MAX)
        return -1;

    M_DEBUG("patch_existing_rpc: sid=0x%x sd=%p old_func=%p\n",
            sid, sd, sd->function);

    rpc_shim_table[rpc_shim_count].sid = sid;
    rpc_shim_table[rpc_shim_count].orig = sd->function;
    rpc_shim_count++;

    /* In-place swap of the function pointer. */
    sd->function = (void *(*)(int, void *, int))thunk;

    return 0;
}

#if 0 //def DEBUG // Too much text output, enable when needed
//--------------------------------------------------------------
static void print_libs()
{
    ModuleInfo_t *m = GetLoadcoreInternalData()->image_info;
    M_DEBUG("Module list:\n");
    M_DEBUG("  name                   |    start |   text |  data |   bss\n");
    while (m != NULL) {
        M_DEBUG("  %-22s | 0x%6x | %6d | %5d | %5d\n", m->name, m->text_start, m->text_size, m->data_size, m->bss_size);
        m = m->next;
    }
}
#else
static inline void print_libs() {}
#endif

#ifdef DEBUG
//--------------------------------------------------------------
static void print_args(int arg_len, char *args)
{
    // Multiple null terminated strings together
    int args_idx = 0;
    int was_null = 1;

    if (arg_len == 0)
        return;

    M_DEBUG("Module arguments (arg_len=%d):\n", arg_len);

    // Search strings
    while(args_idx < arg_len) {
        if (args[args_idx] == 0) {
            if (was_null == 1) {
                M_DEBUG("- args[%d]=0\n", args_idx);
            }
            was_null = 1;
        }
        else if (was_null == 1) {
            M_DEBUG("- args[%d]='%s'\n", args_idx, &args[args_idx]);
            was_null = 0;
        }
        args_idx++;
    }
}
#else
static inline void print_args(int arg_len, char *args) {}
#endif

//--------------------------------------------------------------
static struct FakeModule *checkFakemodByFile(const char *path, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (strstr(path, fakemod_list->fname)) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static struct FakeModule *checkFakemodByName(const char *modname, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (strstr(modname, fakemod_list->name)) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static struct FakeModule *checkFakemodById(int id, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (id == fakemod_list->id) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static int Hook_LoadStartModule(char *modpath, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    print_libs();
    M_DEBUG("%s(%s, %d, ...)\n", __FUNCTION__, modpath, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodByFile(modpath, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            *modres = mod->returnStart;
            rv = mod->id;
        }
        else {
            // Fake module load error
            rv = mod->returnLoad;
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x), modres=%d\n", mod->id, rv, rv, *modres);
        return rv;
    }

    return org_LoadStartModule(modpath, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_StartModule(int id, char *modname, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x, %s, %d, ...)\n", __FUNCTION__, id, modname, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            *modres = mod->returnStart;
            rv = mod->id;
        }
        else {
            // Fake cannot start a module that is not loaded
            rv = -202; // KE_UNKNOWN_MODULE
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x), modres=%d\n", mod->id, rv, rv, *modres);
        return rv;
    }

    return org_StartModule(id, modname, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_LoadModuleBuffer(void *ptr)
{
    struct FakeModule *mod;
    elf_header_t *eh = (elf_header_t *)ptr;
    elf_pheader_t *eph = (elf_pheader_t *)(ptr + eh->phoff);
    const char *modname = (const char *)ptr + eph->offset + 0x1a;

    print_libs();
    M_DEBUG("%s(0x%x) modname = '%s'\n", __FUNCTION__, ptr, modname);

    mod = checkFakemodByName(modname, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            rv = mod->id;
        }
        else {
            // Fake module load error
            rv = mod->returnLoad;
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x)\n", mod->id, rv, rv);
        return rv;
    }

    return org_LoadModuleBuffer(ptr);
}

//--------------------------------------------------------------
static int Hook_StopModule(int id, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x, %d, ...)\n", __FUNCTION__, id, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        M_DEBUG("- FAKING! id=0x%x\n", mod->id);

        if ((mod->prop & FAKE_PROP_UNLOAD) == 0)
            *modres = MODULE_NO_RESIDENT_END;
        else
            org_StopModule(org_SearchModuleByName(mod->name), arg_len, args, modres);

        return mod->id;
    }

    return org_StopModule(id, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_UnloadModule(int id)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        M_DEBUG("- FAKING! id=0x%x\n", mod->id);

        if ((mod->prop & FAKE_PROP_UNLOAD) != 0)
            org_UnloadModule(org_SearchModuleByName(mod->name));

        return mod->id;
    }

    return org_UnloadModule(id);
}

//--------------------------------------------------------------
static int Hook_SearchModuleByName(char *modname)
{
    struct FakeModule *mod;

    M_DEBUG("%s(%s)\n", __FUNCTION__, modname);

    mod = checkFakemodByName(modname, fmd.fake);
    if (mod != NULL) {
        int rv = mod->id;
        if (mod->returnLoad != 0 || mod->returnStart == MODULE_NO_RESIDENT_END)
            rv = -202; // KE_UNKNOWN_MODULE
        M_DEBUG("- FAKING! id=0x%x rv=%d(0x%x)\n", mod->id, rv, rv);
        return rv;
    }

    return org_SearchModuleByName(modname);
}

//--------------------------------------------------------------
static int Hook_ReferModuleStatus(int id, ModuleStatus *status)
{
    struct FakeModule *mod;

    //M_DEBUG("%s(0x%x, ...)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL && mod->returnLoad == 0) {
        //M_DEBUG("- FAKING! id=0x%x\n", mod->id);
        memset(status, 0, sizeof(ModuleStatus));
        strcpy(status->name, mod->name);
        status->version = mod->version;
        status->id = mod->id;
        return id;
    }

    return org_ReferModuleStatus(id, status);
}

//--------------------------------------------------------------
int _start(int argc, char **argv)
{
    int i;

    print_libs();

    // Change string index to string pointers
    M_DEBUG("Fake module list:\n");
    M_DEBUG("         fname | name           | vers. |  rl | rs | prop\n");
    for (i = 0; i < MODULE_SETTINGS_MAX_FAKE_COUNT; i++) {
        struct FakeModule *fm = &fmd.fake[i];

        // Transform file name index to pointer
        if ((unsigned int)fm->fname >= 0x80000000) {
            unsigned int idx = (unsigned int)fm->fname - 0x80000000;
            fm->fname = (char *)&fmd.data[idx];
        }

        // Transform module name index to pointer
        if ((unsigned int)fm->name >= 0x80000000) {
            unsigned int idx = (unsigned int)fm->name - 0x80000000;
            fm->name = (char *)&fmd.data[idx];
        }

        if (fm->fname != NULL) {
            M_DEBUG("  %12s | %-14s | 0x%3x | %3d | %2d | 0x%x\n", fm->fname, fm->name, fm->version, fm->returnLoad, fm->returnStart, fm->prop);
        }
    }

    iop_library_t * lib_modload = ioplib_getByName("modload\0");
    org_LoadStartModule  = ioplib_hookExportEntry(lib_modload,  7, Hook_LoadStartModule);
    org_StartModule      = ioplib_hookExportEntry(lib_modload,  8, Hook_StartModule);
    org_LoadModuleBuffer = ioplib_hookExportEntry(lib_modload, 10, Hook_LoadModuleBuffer);
    // check modload version
    if (lib_modload->version > 0x102) {
        org_ReferModuleStatus  = ioplib_hookExportEntry(lib_modload, 17, Hook_ReferModuleStatus);
        org_StopModule         = ioplib_hookExportEntry(lib_modload, 20, Hook_StopModule);
        org_UnloadModule       = ioplib_hookExportEntry(lib_modload, 21, Hook_UnloadModule);
        org_SearchModuleByName = ioplib_hookExportEntry(lib_modload, 22, Hook_SearchModuleByName);
    } else {
        struct FakeModule *modlist;
        // Change all REMOVABLE END values to RESIDENT END, if modload is old.
        for (modlist = fmd.fake; modlist->fname != NULL; modlist++) {
            if (modlist->returnStart == MODULE_REMOVABLE_END)
                modlist->returnStart = MODULE_RESIDENT_END;
        }
    }

    ioplib_relinkExports(lib_modload);

    /* ---------------------------------------------------------------
     * agent-C: install RPC fallback shim for FILEIO / IOPMEM so that
     * unrecognized RPC codes return success instead of NULL (which
     * causes the EE caller to wait forever).
     *
     * Two complementary mechanisms:
     *   1) Hook sceSifRegisterRpc so any future registrations for the
     *      sids we care about are wrapped at registration time.
     *   2) Best-effort patch of any registrations that already happened
     *      before we got here (FILEIO_service / sce_iopmem from IOPRP
     *      register in their _start before fakemod runs).
     * --------------------------------------------------------------- */
    iop_library_t *lib_sifcmd = ioplib_getByName("sifcmd\0\0");
    if (lib_sifcmd != NULL) {
        /* sceSifRegisterRpc is export 14 in PS2SDK sifcmd. */
        if (ioplib_getTableSize(lib_sifcmd) > 14) {
            org_sceSifRegisterRpc = ioplib_hookExportEntry(lib_sifcmd, 14,
                                                          Hook_sceSifRegisterRpc);
            ioplib_relinkExports(lib_sifcmd);
            M_DEBUG("fakemod: hooked sceSifRegisterRpc, org=%p\n",
                    org_sceSifRegisterRpc);
        }

        /* Patch already-registered services. */
        patch_existing_rpc(lib_sifcmd, 0x80000001); /* FILEIO_service */
        patch_existing_rpc(lib_sifcmd, 0x80000003); /* sce_iopmem */
    } else {
        M_DEBUG("fakemod: sifcmd not found, RPC shim disabled\n");
    }

    return MODULE_RESIDENT_END;
}
