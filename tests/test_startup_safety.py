"""Execute bounded module lookup and startup policy using host-side mocks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.c").read_text()
assert main.index('draw_startup("STARTING");') < main.index('SifInitRpc(0);')
assert "SifSearchModuleByName" not in main
assert "TerminateThread" not in main
assert "current_thread.current_priority + 1" in main
failure = main[main.index("if (pad_start_result < 0)"):
               main.index("revision = select_revision")]
assert "for (;;)" in failure and "DelayThread(1000000)" in failure
assert "return" not in failure

with tempfile.TemporaryDirectory() as temporary:
    temp = Path(temporary)
    (temp / "smem.h").write_text(
        "unsigned int smem_read(void *, void *, unsigned int);\n")
    (temp / "lookup_test.c").write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "iop_module_lookup.h"
static unsigned char ram[0x200000];
static unsigned int reads;
unsigned int smem_read(void *address,void *out,unsigned int size) {
    uintptr_t pos=(uintptr_t)address;
    assert(pos+size<=sizeof(ram)); ++reads;
    memcpy(out,ram+pos,size); return size;
}
static void module(uint32_t at,uint32_t next,uint32_t name,uint16_t id) {
    memcpy(ram+at,&next,4); memcpy(ram+at+4,&name,4);
    memcpy(ram+at+12,&id,2);
}
int main(void) {
    module(0x800,0x900,0x1000,3); strcpy((char*)ram+0x1000,"padman");
    module(0x900,0,0x1100,4); strcpy((char*)ram+0x1100,"mmceman");
    assert(iop_module_find("padman")==3);
    assert(iop_module_find("mmceman")==4);
    assert(iop_module_find("pad")==-ENOENT);
    assert(iop_module_find("missing")==-ENOENT);
    assert(iop_module_find(0)==-EINVAL);
    module(0x900,0x800,0x1100,4); reads=0;
    assert(iop_module_find("missing")==-ELOOP && reads<=512);
    module(0x800,0x900,0x80200000,3);
    assert(iop_module_find("mmceman")==4);
    module(0x800,0x80200000,0x80001000,3);
    assert(iop_module_find("padman")==3);
    assert(iop_module_find("missing")==-EIO);
    module(0x800,0x901,0x1000,3);
    assert(iop_module_find("missing")==-EIO);
    return 0;
}
''')
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(temp), "-I" + str(root / "include"),
                    str(root / "src/iop_module_lookup.c"), str(temp / "lookup_test.c"),
                    "-o", str(temp / "lookup_test")], check=True)
    subprocess.run([str(temp / "lookup_test")], check=True)

    startup = main[main.index("typedef struct pad_diagnostics"):
                   main.index("static void suspend_pad_for_source_drivers")]
    (temp / "startup_test.c").write_text(r'''
#include <assert.h>
#include <string.h>
#include <errno.h>
#define PROGRAM_TITLE "Test"
typedef struct { void *func,*stack,*gp_reg; int stack_size,initial_priority; } ee_thread_t;
typedef struct { int current_priority; } ee_thread_status_t;
static void (*worker)(void *);
static int resident,mmce=1,scenario,loads,opens,waits,prepare_error;
void *_gp;
int source_media_is_mmce(void) { return mmce; }
int source_media_prepare_mmce(volatile int *stage) {
    if(!mmce)return 0;
    *stage=10; if(prepare_error)return prepare_error; resident=1; return 0;
}
int source_media_prepare_controller_stack(volatile int *stage) {
    return source_media_prepare_mmce(stage);
}
int source_media_is_selected(void) { return 1; }
const char *source_media_label(void) { return "MMCE"; }
int iop_module_find(const char *name) { (void)name; return resident ? 5 : -2; }
int SifLoadModule(const char *p,int n,void *a) { (void)p;(void)n;(void)a;++loads;return 5; }
int padInit(int n) { (void)n;return scenario==2 ? -1 : 1; }
int padPortOpen(int p,int s,void *b) { (void)p;(void)s;(void)b;++opens;return 1; }
void ExitDeleteThread(void) {}
void ui_begin(void) {}
void ui_printf(const char *s,...) { (void)s; }
void ui_inverse_status(const char *s) { (void)s; }
void ui_sync(void) {}
int GetThreadId(void) { return 1; }
int ReferThreadStatus(int id,ee_thread_status_t *s) { (void)id;s->current_priority=64;return 0; }
int CreateThread(ee_thread_t *t) { assert(t->initial_priority==65); worker=t->func;return 2; }
int StartThread(int id,void *p) { (void)id;if(scenario==3)return -1;if(scenario!=1)worker(p);return 0; }
int DeleteThread(int id) { (void)id;return 0; }
int DelayThread(int n) { (void)n;++waits;return 0; }
''' + startup + r'''
int main(void) {
    pad_diagnostics_t pad;
    draw_startup("STARTING");
    resident=1;
    assert(initialize_pad_with_status(&pad)==0 && loads==0 && opens==1);
    resident=0; loads=opens=0;
    assert(initialize_pad_with_status(&pad)==0 && loads==0 && opens==1);
    prepare_error=-19; loads=opens=0;
    assert(initialize_pad_with_status(&pad)==-EIO && opens==0 && loads==0);
    prepare_error=0;
    resident=0; mmce=0; loads=opens=0;
    assert(initialize_pad_with_status(&pad)==0 && loads==2);
    mmce=1; scenario=2; opens=0;
    assert(initialize_pad_with_status(&pad)==-EIO && opens==0);
    scenario=3;
    assert(initialize_pad_with_status(&pad)==-EIO);
    scenario=1; waits=0;
    assert(initialize_pad_with_status(&pad)==-ETIMEDOUT && waits==1000);
    return 0;
}
''')
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(temp / "startup_test.c"), "-o", str(temp / "startup_test")], check=True)
    subprocess.run([str(temp / "startup_test")], check=True)
print("Bounded module lookup and pad startup/timeout policy: PASS (host mocks)")
