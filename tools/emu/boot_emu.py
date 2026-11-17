import sys, struct, time
from unicorn import *
from unicorn.arm_const import *

img = open(sys.argv[1], 'rb').read()
maxins = int(sys.argv[2]) if len(sys.argv) > 2 else 400_000_000
FD_BASE, FD_SIZE = 0x10008000, 0x200000

mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
mu.mem_map(0x00000000, 0x40000000)          # 1 GiB DRAM
mu.mem_map(0xd0000000, 0x05000000)          # MPCore + APB/AXI peripherals (RAM-backed)
mu.mem_write(0x30000000, img)               # as left by U-Boot fastboot

UART_THR, UART_LSR = 0xd4017000, 0xd4017014
TMR_CVWR1 = 0xd4014000 + 0xa4 + 4
mu.mem_write(UART_LSR, struct.pack('<I', 0x61))   # THRE|TEMT, DR
mu.mem_write(0xd1dff004, struct.pack('<I', 0x23))  # GICD_TYPER: 128 IRQs, 2 CPUs
mu.mem_write(0xd1dfe10c, struct.pack('<I', 0x3ff)) # GICC_IAR: spurious
out = bytearray()
cnt = [0]

def on_write(uc, access, addr, size, value, ud):
    if addr == UART_THR:
        out.append(value & 0xff)
        if value == 10:
            sys.stdout.write(out.decode('latin1')); sys.stdout.flush(); out.clear()
    if addr == UART_LSR:
        pass

def on_read(uc, access, addr, size, value, ud):
    if addr == TMR_CVWR1:
        cnt[0] += 40                                  # time advances on every read
        uc.mem_write(TMR_CVWR1, struct.pack('<I', cnt[0] & 0xffffffff))
    elif addr == UART_LSR:
        uc.mem_write(UART_LSR, struct.pack('<I', 0x60))   # always ready, no input

mu.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0xd4017000, end=0xd4017fff)
mu.hook_add(UC_HOOK_MEM_READ, on_read, begin=0xd4014000, end=0xd4017fff)

last = {}
def on_intr(uc, intno, ud):
    pc = uc.reg_read(UC_ARM_REG_PC)
    print(f"\n*** CPU exception intno={intno} pc={pc:#x} lr={uc.reg_read(UC_ARM_REG_LR):#x}")
    uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, on_intr)

t = time.time()
try:
    mu.emu_start(0x30000800, 0, count=maxins)
except UcError as e:
    print(f"\n*** emulation error: {e} pc={mu.reg_read(UC_ARM_REG_PC):#x}")
sys.stdout.write(out.decode('latin1'))
print(f"\n--- stopped at pc={mu.reg_read(UC_ARM_REG_PC):#x} after {time.time()-t:.0f}s")

from PIL import Image
fb = bytes(mu.mem_read(0x17000000, 1024*600*4))
Image.frombytes('RGBA', (1024, 600), fb, 'raw', 'BGRA').convert('RGB').save(sys.argv[3] if len(sys.argv) > 3 else '/tmp/emu/fb.png')
