import sys, struct
from unicorn import *
from unicorn.arm_const import *

img = open(sys.argv[1], 'rb').read()
entry_expected = int(sys.argv[2], 16)
FD_BASE, FD_SIZE = 0x10008000, 0x200000

def run(load_addr, label):
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    mu.mem_map(0x10000000, 0x01000000)      # DRAM window around FD_BASE
    mu.mem_map(0x30000000, 0x01000000)      # fastboot buffer
    mu.mem_write(0x30000000, img)           # boot.img as fastboot stores it
    payload = img[2048:2048 + FD_SIZE]
    start = load_addr
    if load_addr == FD_BASE:                # S-Boot case: payload already at FD_BASE
        mu.mem_write(FD_BASE, payload)
    mu.reg_write(UC_ARM_REG_R0, 0); mu.reg_write(UC_ARM_REG_R1, 0xffffffff); mu.reg_write(UC_ARM_REG_R2, 0x12345678)
    hit = {}
    def hook(uc, addr, size, ud):
        if addr == entry_expected:
            hit['pc'] = addr; uc.emu_stop()
    mu.hook_add(UC_HOOK_CODE, hook)
    try:
        mu.emu_start(start, 0, count=5_000_000)
    except UcError as e:
        print(label, 'emulation error', e, 'pc=%#x' % mu.reg_read(UC_ARM_REG_PC)); return False
    ok_pc = hit.get('pc') == entry_expected
    ok_copy = bytes(mu.mem_read(FD_BASE, FD_SIZE)) == payload
    print(f"{label}: entered at {start:#x} -> reached SEC entry {entry_expected:#x}: {ok_pc}; "
          f"FD at {FD_BASE:#x} matches: {ok_copy}; r0-r2 = "
          f"{mu.reg_read(UC_ARM_REG_R0):#x} {mu.reg_read(UC_ARM_REG_R1):#x} {mu.reg_read(UC_ARM_REG_R2):#x}")
    return ok_pc and ok_copy

a = run(FD_BASE, "S-Boot   ")
b = run(0x30000800, "fastboot ")
sys.exit(0 if a and b else 1)
