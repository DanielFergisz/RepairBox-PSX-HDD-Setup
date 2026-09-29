"""Run the real preflight renderer with memory snapshots and a text UI stub."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.c").read_text()
renderer = main[main.index("static const char *psx2_storage_status"):
                main.index("static void psx2_progress_callback")]
psx1_renderer = main[main.index("static void draw_preflight_wait"):
                     main.index("static void draw_psx1_source_wait")]
assert "fileXio" not in renderer
assert "iop_module_find" not in renderer
assert "fileXio" not in psx1_renderer
assert "source_media_select" not in psx1_renderer
assert main.count('"CHECKING STORAGE AND PACKAGE"') == 2
assert main.count('"CHECKING SYSTEM PACKAGE"') == 2
assert "details ^= 1;" in main
with tempfile.TemporaryDirectory() as folder:
    temp = Path(folder)
    (temp / "tamtypes.h").write_text(
        "#pragma once\n#include <stdint.h>\n"
        "typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;"
        "typedef unsigned long long u64; typedef int32_t s32;\n")
    harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "direct_ready40.h"
#include "activation.h"
#include "xfrom_repair.h"
#include "capacity_profile.h"
#include "psx1_pipeline.h"
#include "ui.h"
#define PROGRAM_TITLE "RepairBox.pl PSX HDD + Apps Setup v1.2-RC11"
static char screen[8192];
static int x, y;
void ui_begin(void) { screen[0]=0; x=40; y=16; }
void ui_printf(const char *fmt,...) {
    char text[2048]; va_list ap; va_start(ap,fmt);
    vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    assert(strlen(screen)+strlen(text)<sizeof(screen)); strcat(screen,text);
    for (const char *p=text; *p; ++p) {
        if (*p=='\n') { x=40; y+=12; }
        else { if (x+8>600) { x=40; y+=12; } assert(y<=200); x+=10; }
    }
}
void ui_set_position(int a,int b) { x=a; y=b; }
void ui_inverse_status(const char *s) { ui_printf("%s\n",s); }
void ui_sync(void) {}
static const char *source_label = "MMCE 1";
const char *source_media_label(void) { return source_label; }
const char *psx1_capacity_name(media_capacity_class_t c) { (void)c; return "256 GB"; }
const char *format_test_state_name(pre_format_state_t s) { (void)s; return "VALID APA / PFS"; }
const char *source_media_psx2_system_root(void) {
    return "mmce0:/RepairBox-PSX2-SystemFiles";
}
int source_media_driver_result(void) { return 0; }
int source_media_resolution_result(void) { return 0; }
const char *source_media_resolution_name(void) { return "READY"; }
const char *capacity_profile_name(capacity_profile_t p) { (void)p; return "256 GB"; }
const char *bootflag_ro_state_name(bootflag_ro_state_t s) { (void)s; return "NORMAL"; }
'''
    harness += psx1_renderer + renderer + r'''
int main(void) {
    static psx1_result_t p;
    static xfrom_repair_result_t xfrom;
    xfrom.source_valid=1;
    xfrom.xfrom_partition_accessible=1;
    const char *sources[] = {"USB", "MX4SIO", "MMCE 1", "MMCE 2", "DETECTING"};
    for (unsigned int i=0;i<sizeof(sources)/sizeof(sources[0]);++i) {
        source_label = sources[i];
        draw_preflight_wait("PSX1 - First Revision", "CHECKING STORAGE AND PACKAGE");
        assert(strstr(screen, source_label));
        assert(strstr(screen, "No disk write has started."));
        draw_preflight_wait("PSX2 - Second Revision", "CHECKING STORAGE AND PACKAGE");
        assert(strstr(screen, source_label));
        draw_psx1_preflight(&p,&xfrom,0);
        assert(strstr(screen, source_label));
        assert(strstr(screen, "System files   NOT CHECKED"));
    }
    source_label = "MMCE 1";
    p.installer.source_scan_attempted=1;
    p.installer.failure_return=-19;
    memset(p.installer.failure_operation,'A',sizeof(p.installer.failure_operation)-1);
    draw_psx1_preflight(&p,&xfrom,0);
    assert(strstr(screen,"System files   FAILED"));
    draw_psx1_preflight(&p,&xfrom,1);
    assert(strstr(screen,"(-19)"));
    assert(strstr(screen,"LEFT/RIGHT: Back   TRIANGLE: Rescan"));
    p.format.capacity_class=MEDIA_CAPACITY_SETMAX_HIDDEN;
    draw_psx1_preflight(&p,&xfrom,1);
    p.package_ready=1;
    p.preflight_valid=1;
    draw_psx1_preflight(&p,&xfrom,0);
    assert(strstr(screen,"System files   READY"));
    assert(strstr(screen,"Hold L1 + R1 and press X."));
    dr40_result_t r={0}; activation_result_t a={0};
    storage_diagnostics_t d={0};
    r.mode=DR40_MODE_INITIALIZE;
    for(int i=0;i<STORAGE_MODULE_COUNT;i++) {
        d.modules[i].name="usbhdfsd"; d.modules[i].module_id=32+i;
        d.modules[i].module_result=2147483647;
    }
    draw_psx2_preflight(&r,&a,&xfrom,CAPACITY_PROFILE_256_VERIFIED,0,&d);
    assert(strstr(screen,"System files   NOT CHECKED"));
    assert(strstr(screen,"Bootstrap      NOT CHECKED"));
    r.installer.source_scan_attempted=1; r.installer.failure_return=-19;
    strcpy(r.installer.failure_operation,"source_root_dopen");
    r.installer.source_root_open_result=-19;
    r.bootstrap.validation_attempted=1; r.bootstrap.binary_open_result=-19;
    draw_psx2_preflight(&r,&a,&xfrom,CAPACITY_PROFILE_256_VERIFIED,0,&d);
    assert(strstr(screen,"System files   FAILED"));
    for(int page=1;page<2;page++) {
        draw_psx2_preflight(&r,&a,&xfrom,CAPACITY_PROFILE_256_VERIFIED,page,&d);
        if(page==1) {
            assert(strstr(screen,"source_root_dopen"));
            assert(strstr(screen,"Package error: -19"));
            assert(strstr(screen,"Bootstrap: NOT READY"));
        }
    }
    r.source_ready=1; r.bootstrap_ready=1;
    xfrom.source_valid=1; xfrom.xfrom_root_accessible=1;
    draw_psx2_preflight(&r,&a,&xfrom,CAPACITY_PROFILE_256_VERIFIED,0,&d);
    assert(strstr(screen,"System files   READY"));
    assert(strstr(screen,"Bootstrap      READY"));
    return 0;
}
'''
    (temp / "test.c").write_text(harness)
    subprocess.run(["cc", "-D_EE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I"+str(temp), "-I"+str(root/"include"),
                    str(temp/"test.c"), "-o", str(temp/"test")], check=True)
    subprocess.run([str(temp/"test")], check=True)
print("Preflight snapshot renderer, status distinctions and layout: PASS (host)")
