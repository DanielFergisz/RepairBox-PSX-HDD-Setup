#ifndef REPAIRBOX_IOP_MODULE_LOOKUP_H
#define REPAIRBOX_IOP_MODULE_LOOKUP_H

/* Read-only, bounded module-list lookup; does not call LOADFILE RPC. */
int iop_module_find(const char *name);

#endif
