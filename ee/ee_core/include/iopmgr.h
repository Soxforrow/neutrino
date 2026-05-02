#ifndef IOPMGR_H
#define IOPMGR_H


void services_start(void);
void services_exit(void);

void New_Reset_Iop(const char *arg, int arglen);
void New_Reset_Iop2(const char *arg, int arglen, int eeload);

// agent-K: pre-load game-specific IOP modules from disc after IOP reset
int iopmgr_preload_game_modules(const char *iso_path);

void Install_Kernel_Hooks(void);


#endif
