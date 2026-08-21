# RepairBox.pl PSX HDD Setup v1.0

RepairBox.pl PSX HDD Setup prepares replacement storage for Sony PSX DESR
consoles. It supports both PSX hardware revisions from one ELF and is designed
primarily for the RepairBox FC1307A IDE-to-SATA adapter.

The application is destructive: selecting a revision erases and rebuilds the
attached PSX storage device.

## Supported hardware

- PSX1 — DESR-5000, DESR-5100, DESR-7000 and DESR-7100.
- PSX2 — DESR-5500, DESR-5700, DESR-7500 and DESR-7700.
- PSX1 media verified: 64 GB, 128 GB, 256 GB, 512 GB and 1 TB.
- PSX2 media verified: 256 GB, 512 GB and 1 TB.
- 32 GB media is not supported.
- PSX2 media smaller than 256 GB is not supported.

Use `[ L1 ]` for PSX1 or `[ R1 ]` for PSX2 on the revision selection screen.

The ELF must be launched with `wLaunchELF v4.70_R3Z`. Standard wLaunchELF
builds do not provide the required DVR and XFROM support.

## Build

A working PS2DEV/PS2SDK environment is required. From the project directory:

```sh
export PS2DEV=/path/to/ps2dev
export PS2SDK="$PS2DEV/ps2sdk"
export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
make clean all
```

Alternatively, after setting `PS2DEV` and `PS2SDK`:

```sh
sh tools/build_local.sh
```

The resulting file is `RepairBox.pl-PSX-HDD-Setup-v1.0.elf`. A verified
prebuilt ELF and its SHA-256 manifest are also provided in `release/`.

The bundled IRX files and `libmc-xfrom.a` are required for compilation. The
generated `*_irx.c` files and object files are build artifacts and are not
stored in the repository.

## USB layout

PSX1 system files:

```text
mass:/RepairBox-PSX1-SystemFiles/
    __net/
    __system/
    __sysconf/
    __common/
```

PSX2 system files and bootstrap:

```text
mass:/RepairBox-PSX2-SystemFiles/
    __net/
    __system/
    __sysconf/
    __common/
    __xdata/
    __xcontents/

mass:/RepairBox-PSX2-Bootstrap/
    mbr_bootstrap_prefix.bin
    SHA256SUMS.txt
```

System files and proprietary Sony data are not distributed with this source.

## PSX2 bootstrap requirement

`src/bootstrap.c` and `include/bootstrap.h` are normal source files and are
compiled into the ELF. They contain the code that reads, validates, writes and
verifies a bootstrap package.

`mbr_bootstrap_prefix.bin` is external runtime data. It is not embedded in the
ELF, is not distributed with this repository and is not needed to compile the
project. It is required on the USB drive only when running the PSX2 setup path.
PSX1 does not use it.

## Implementation notes

PSX1 uses the fast APA formatter, creates the required five APA entries and
four PFS volumes, then copies and verifies the system files. It does not use
DVR, SETMAX, XFROM or the PSX2 MBR bootstrap.

PSX2 preserves the verified SETMAX, full cold boot, DirectReady40, DVR,
bootstrap, system copy, XFROM 40/1 activation and readback sequence.
