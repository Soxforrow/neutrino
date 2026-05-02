#ifndef IOPMGR_H
#define IOPMGR_H


void services_start(void);
void services_exit(void);

void New_Reset_Iop(const char *arg, int arglen);
void New_Reset_Iop2(const char *arg, int arglen, int eeload);

// agent-K2: pre-load game-specific IOP modules from disc after IOP reset
// Returns count of modules successfully loaded.
int iopmgr_preload_game_modules(void);

void Install_Kernel_Hooks(void);


#endif
