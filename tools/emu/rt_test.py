"""Boot the firmware in Unicorn up to BDS, then do what Windows does with UEFI:
GetMemoryMap -> ExitBootServices -> SetVirtualAddressMap (runtime regions moved to new
virtual addresses), switch to a page table that maps ONLY the runtime regions at their new
addresses, and call every runtime service Windows uses. Any access to an address the OS would
not have mapped (an unconverted pointer, undeclared MMIO) raises an abort that is reported.

  python3 rt_test.py goyawifi-uefi-winrt-debug.img
"""
import sys, struct, time, subprocess, os
from unicorn import *
from unicorn.arm_const import *

img = open(sys.argv[1], 'rb').read()
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
mu.mem_map(0x00000000, 0x40000000)
mu.mem_map(0xd0000000, 0x05000000)
mu.mem_write(0x30000000, img)
UART_THR, UART_LSR = 0xd4017000, 0xd4017014
TMR_CVWR1 = 0xd4014000 + 0xa4 + 4
mu.mem_write(UART_LSR, struct.pack('<I', 0x61))
mu.mem_write(0xd1dff004, struct.pack('<I', 0x23))
mu.mem_write(0xd1dfe10c, struct.pack('<I', 0x3ff))
mu.mem_write(0xd4208100, struct.pack('<I', 0x01000040))
mu.mem_write(0xd4208104, struct.pack('<I', 0x00000001))
out = bytearray(); cnt = [0]; phase = ['boot']; log = []

def on_write(uc, access, addr, size, value, ud):
    if addr == UART_THR:
        out.append(value & 0xff)
        if value == 10:
            line = out.decode('latin1'); out.clear(); log.append(line)
            if phase[0] != 'boot': sys.stdout.write('  fw: ' + line)
            if 'BdsWait (' in line or 'BdsWait(' in line:
                uc.emu_stop()
def on_read(uc, access, addr, size, value, ud):
    if addr == TMR_CVWR1:
        cnt[0] += 40
        uc.mem_write(TMR_CVWR1, struct.pack('<I', cnt[0] & 0xffffffff))
    elif addr == UART_LSR:
        uc.mem_write(UART_LSR, struct.pack('<I', 0x60))
USBCMD, USBSTS = 0xd4208140, 0xd4208144
def on_usb_read(uc, access, addr, size, value, ud):
    if addr == USBCMD:
        v = struct.unpack('<I', uc.mem_read(USBCMD, 4))[0]
        uc.mem_write(USBCMD, struct.pack('<I', v & ~2))
        uc.mem_write(USBSTS, struct.pack('<I', 0x1000 if not (v & 1) else 0))
mu.hook_add(UC_HOOK_MEM_READ, on_usb_read, begin=USBCMD, end=USBCMD + 3)
mu.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=0xd4017000, end=0xd4017fff)
mu.hook_add(UC_HOOK_MEM_READ, on_read, begin=0xd4014000, end=0xd4017fff)

fault = []
def cp(crn, crm, op1, op2):
    return mu.reg_read(UC_ARM_REG_CP_REG, (15, 0, 0, crn, crm, op1, op2))
def on_intr(uc, intno, ud):
    names = {1: 'undefined instruction', 2: 'SVC', 3: 'prefetch abort', 4: 'data abort'}
    fault.append((intno, uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_LR)))
    uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, on_intr)

t = time.time()
mu.emu_start(0x30000800, 0, count=6_000_000_000)
print(f'reached BDS after {time.time()-t:.0f}s, pc={mu.reg_read(UC_ARM_REG_PC):#x}')
if fault: print('fault during boot', fault); sys.exit(1)
phase[0] = 'harness'

rd32 = lambda a: struct.unpack('<I', mu.mem_read(a, 4))[0]
wr32 = lambda a, v: mu.mem_write(a, struct.pack('<I', v & 0xffffffff))

# find the system table
ram = bytes(mu.mem_read(0x3c000000, 0x04000000))
st = None
i = 0
while True:
    i = ram.find(b'IBI SYST', i)
    if i < 0: break
    a = 0x3c000000 + i
    bs, rt = rd32(a + 60), rd32(a + 56)
    ok = 0 < bs < 0x40000000 and 0 < rt < 0x40000000 and mu.mem_read(bs, 8) == b'BOOTSERV' and mu.mem_read(rt, 8) == b'RUNTSERV'
    print(f'  candidate {a:#x} BS={bs:#x} RT={rt:#x} ok={ok}')
    if ok: st = a
    i += 8
print(f'gST={st:#x}')
BS, RT = rd32(st + 60), rd32(st + 56)

# scratch: PA 0x20000000 identity (stub + sentinel), PA 0x20100000 = VA 0xA0000000 (data, stack)
stub = subprocess.run(['arm-linux-gnueabihf-as', '-o', '/tmp/emu/stub.o', '/tmp/emu/stub.s'], check=True)
subprocess.run(['arm-linux-gnueabihf-objcopy', '-O', 'binary', '/tmp/emu/stub.o', '/tmp/emu/stub.bin'], check=True)
mu.mem_write(0x20000000, open('/tmp/emu/stub.bin', 'rb').read())
SENT = 0x20000100
DATA_PA, DATA_VA = 0x20100000, 0xA0000000
STACK_PA = DATA_PA + 0xF0000

def call(fn, args, stack_pa, stack_va, budget=50_000_000):
    """AAPCS call; stack args at stack top. Returns r0 or None on fault/timeout."""
    sp_va = stack_va - 0x100
    for k, a in enumerate(args[4:]):
        mu.mem_write(stack_pa - 0x100 + 4 * k, struct.pack('<I', a & 0xffffffff))
    regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
    for r, a in zip(regs, args[:4]): mu.reg_write(r, a & 0xffffffff)
    mu.reg_write(UC_ARM_REG_SP, sp_va)
    mu.reg_write(UC_ARM_REG_LR, SENT)
    fault.clear()
    try:
        mu.emu_start(fn, SENT, count=budget)
    except UcError as e:
        print(f'    emulation error {e} pc={mu.reg_read(UC_ARM_REG_PC):#x}'); return None
    if fault:
        n, pc, lr = fault[0]
        print(f'    *** CPU EXCEPTION {n} ({ {1:"undef",3:"prefetch abort",4:"data abort"}.get(n,n) }) '
              f'pc={pc:#x} lr={lr:#x} DFAR={cp(6,0,0,0):#x} DFSR={cp(5,0,0,0):#x} IFAR={cp(6,0,0,2):#x}')
        return None
    if mu.reg_read(UC_ARM_REG_PC) != SENT:
        print(f'    did not return (pc={mu.reg_read(UC_ARM_REG_PC):#x})'); return None
    return mu.reg_read(UC_ARM_REG_R0)

# ---- boot-time part (identity map, data buffers at their PA) ----
B = DATA_PA
pSize, pKey, pDsz, pVer, MAP = B, B + 4, B + 8, B + 12, B + 0x1000
wr32(pSize, 0x8000)
r = call(rd32(BS + 40), [pSize, MAP, pKey, pDsz, pVer], STACK_PA, STACK_PA)
print(f'GetMemoryMap -> {r:#x} size={rd32(pSize)} descsize={rd32(pDsz)}')
r = call(rd32(BS + 128), [0, rd32(pKey)], STACK_PA, STACK_PA, budget=500_000_000)
print(f'ExitBootServices -> {r:#x}' if r is not None else 'ExitBootServices failed')

dsz, n = rd32(pDsz), rd32(pSize) // rd32(pDsz)
def va_of(pa):
    if pa < 0x40000000: return pa + 0x40000000
    if pa >= 0xd0000000: return pa - 0x40000000
    raise ValueError(hex(pa))
names = {5: 'RtCode', 6: 'RtData', 11: 'MMIO', 12: 'MMIOPort'}
l1 = bytearray(16384)
def sect(va, pa, normal):
    struct.pack_into('<I', l1, (va >> 20) * 4, (pa & 0xfff00000) | 0xc02 | (0xc if normal else 0))
rt_ranges = []
for k in range(n):
    d = MAP + k * dsz
    typ = rd32(d); pa = struct.unpack('<Q', mu.mem_read(d + 8, 8))[0]
    pages = struct.unpack('<Q', mu.mem_read(d + 24, 8))[0]
    attr = struct.unpack('<Q', mu.mem_read(d + 32, 8))[0]
    if attr & (1 << 63):
        va = va_of(pa)
        mu.mem_write(d + 16, struct.pack('<Q', va))
        rt_ranges.append((pa, pages, va))
        print(f'  runtime {names.get(typ, typ):>8} PA {pa:#010x} +{pages:4} pages attr {attr:#x} -> VA {va:#010x}')
        for m in range(pa >> 20, ((pa + pages * 4096 - 1) >> 20) + 1):
            sect(va_of(m << 20), m << 20, typ in (5, 6))
sect(0x20000000, 0x20000000, True)
sect(DATA_VA, DATA_PA, True)
L1PA = 0x21000000
mu.mem_write(L1PA, bytes(l1))

r = call(rd32(RT + 40), [n * dsz, dsz, rd32(pVer), MAP], STACK_PA, STACK_PA)
print(f'SetVirtualAddressMap -> {r:#x}' if r is not None else 'SetVirtualAddressMap failed')

# switch to the OS-like page table
mu.reg_write(UC_ARM_REG_R4, L1PA); mu.reg_write(UC_ARM_REG_R6, SENT)
mu.emu_start(0x20000000, SENT, count=100)
print(f'TTBR0 now {cp(2,0,0,0):#x}; only runtime regions are mapped')

# ---- runtime part: everything through virtual addresses ----
RTV = RT + 0x40000000          # RT table lives in runtime data (PA<1G)
assert mu.mem_read(RT, 8) == b'RUNTSERV'
V = lambda pa: pa - DATA_PA + DATA_VA
def svc(off): return rd32(RT + off)
SVA = DATA_VA + 0xF0000
Tm, Buf, Sz, Guid, Name, Attr = DATA_PA + 0x100, DATA_PA + 0x2000, DATA_PA + 0x200, DATA_PA + 0x210, DATA_PA + 0x300, DATA_PA + 0x220
def u16(s): return (s + '\0').encode('utf-16-le')
GV = bytes.fromhex('61dfe48bca93d211aa0d00e098032b8c')   # EFI_GLOBAL_VARIABLE

print('GetTime:'); r = call(svc(24), [V(Tm), 0], STACK_PA, SVA)
if r is not None:
    y, mo, dd, hh, mi, ss = struct.unpack('<HBBBBB', mu.mem_read(Tm, 7))
    print(f'    -> {r:#x}  {y}-{mo:02}-{dd:02} {hh:02}:{mi:02}:{ss:02}')
print('SetTime:'); r = call(svc(28), [V(Tm)], STACK_PA, SVA); print(f'    -> {r}')
print('GetTime again:'); r = call(svc(24), [V(Tm), 0], STACK_PA, SVA); print(f'    -> {r}')
print('GetWakeupTime:'); r = call(svc(32), [V(Sz), V(Sz + 4), V(Tm)], STACK_PA, SVA); print(f'    -> {r if r is None else hex(r)}')
print('GetNextHighMonotonicCount:'); r = call(svc(60), [V(Sz)], STACK_PA, SVA); print(f'    -> {r if r is None else hex(r)}')
print('QueryVariableInfo:'); r = call(svc(76), [7, V(Buf), V(Buf + 8), V(Buf + 16)], STACK_PA, SVA); print(f'    -> {r if r is None else hex(r)}')

print('GetNextVariableName walk:')
mu.mem_write(Name, b'\0\0'); found = []
for _ in range(64):
    wr32(Sz, 512)
    r = call(svc(52), [V(Sz), V(Name), V(Guid)], STACK_PA, SVA)
    if r is None or r != 0:
        print(f'    end -> {r if r is None else hex(r)}'); break
    nm = bytes(mu.mem_read(Name, 512)).decode('utf-16-le').split('\0')[0]
    g = bytes(mu.mem_read(Guid, 16))
    wr32(Sz, 0x1000)
    r2 = call(svc(48), [V(Name), V(Guid), V(Attr), V(Sz), V(Buf)], STACK_PA, SVA)
    found.append(nm)
    print(f'    {nm:<28} GetVariable -> {r2 if r2 is None else hex(r2)} size={rd32(Sz)}')
    if r2 is None: break
print('SetVariable (volatile):')
mu.mem_write(Name, u16('GoyaRtTest')); mu.mem_write(Guid, GV); mu.mem_write(Buf, b'\x11\x22\x33\x44')
r = call(svc(56), [V(Name), V(Guid), 6, 4, V(Buf)], STACK_PA, SVA); print(f'    -> {r if r is None else hex(r)}')
print('SetVariable (non-volatile, like Windows does for BootNext/Timeout/OsIndications):')
mu.mem_write(Name, u16('Timeout')); mu.mem_write(Buf, b'\x05\x00')
r = call(svc(56), [V(Name), V(Guid), 7, 2, V(Buf)], STACK_PA, SVA); print(f'    -> {r if r is None else hex(r)}')
print('done')
