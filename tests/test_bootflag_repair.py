from pathlib import Path

root = Path(__file__).resolve().parents[1]
activation = (root / "src/activation.c").read_text(encoding="utf-8")
main = (root / "src/main.c").read_text(encoding="utf-8")
direct = (root / "src/direct_ready40.c").read_text(encoding="utf-8")
multi = (root / "src/direct_ready40_multi.c").read_text(encoding="utf-8")

# Preserve an already-valid pending image, but never use damaged old payload
# contents as the source of a repair. A readable old file is replaced in full.
assert "result->current_accessible" in activation
assert "result->replacement_required" in activation
assert "!result->current_valid" in activation
assert "bootflag_generate_standard_40gb" in activation
arm = activation[activation.index("int activation_arm_pending("):]
assert "!result->generation_valid ||" in arm
assert "!result->current_accessible" not in arm
assert "FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC" in arm
assert "memcmp(result->pending_readback.bytes, result->generated.bytes" in arm
assert "activation_repair_current_digest" not in activation
assert "REPAIR XFROM CHECKSUM" not in main
assert "REPLACE ON COMPLETE" in main

for pipeline in (direct, multi):
    assert "result->bootflag_accessible" in pipeline
    assert "!result->bootflag_accessible" not in pipeline
    assert "result->final_bootstrap_valid && result->bootflag_accessible" not in pipeline

print("missing or damaged bootflag replacement after XFROM repair: PASS")
