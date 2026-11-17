"""Run the CPU1 stub (Cpu1Stub.S) alone in Unicorn and play CPU0's role:
wait for the alive magic, ping it, check the answer, park it.

usage: cpu1_stub_test.py <Cpu1Stub.obj>
"""
import subprocess, struct, sys
from unicorn import *
from unicorn.arm_const import *

obj = sys.argv[1]
subprocess.check_call(['arm-linux-gnueabi-objcopy', '-O', 'binary', '--only-section=.text', obj, '/tmp/cpu1stub.bin'])
code = open('/tmp/cpu1stub.bin', 'rb').read()
assert len(code) == 0x180, hex(len(code))

BASE = 0x10000000          # a page of "DRAM"
MB = BASE + 0x100

mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
mu.mem_map(BASE, 0x1000)
mu.mem_write(BASE, code)

def rd(off): return struct.unpack('<I', mu.mem_read(MB + off, 4))[0]
def wr(off, v): mu.mem_write(MB + off, struct.pack('<I', v))

mu.reg_write(UC_ARM_REG_CPSR, 0x1d3)            # SVC, as after reset
mu.reg_write(UC_ARM_REG_PC, BASE)

def run(maxins=400):
    pc = mu.reg_read(UC_ARM_REG_PC)
    try:
        mu.emu_start(pc, BASE + 0x1000, count=maxins)
    except UcError as e:
        # Unicorn reports WFE/WFI as an invalid instruction; the stub sleeps
        # there until CPU0 sends an event, so treat it as "halted" and let
        # the next run() resume after it.
        pc = mu.reg_read(UC_ARM_REG_PC)
        op = struct.unpack('<I', mu.mem_read(pc - 4, 4))[0]
        if op not in (0xe320f002, 0xe320f003):
            print('emu error', e, 'pc=%#x' % pc); raise

run()
assert rd(0x00) == 0xA9A9C0DE, 'no alive magic: %#x' % rd(0)
print('alive: MIDR=%#x MPIDR=%#x SCTLR=%#x state=%u' % (rd(4), rd(8), rd(0xc), rd(0x24)))
assert rd(0x24) == 1

# ping
wr(0x44, 0x12345678); wr(0x40, 2)
run()
assert rd(0x18) == 2, 'no ack'
assert rd(0x20) == (~0x12345678) & 0xffffffff, 'bad result %#x' % rd(0x20)
print('ping ok: ack=%u result=%#x heartbeat=%u' % (rd(0x18), rd(0x20), rd(0x10)))

# a second ping with a new sequence number
wr(0x44, 0xdeadbeef); wr(0x40, 3)
run()
assert rd(0x18) == 3 and rd(0x20) == (~0xdeadbeef) & 0xffffffff
print('second ping ok')

# park
wr(0x40, 1)
run()
assert rd(0x24) == 2, 'not parked'
print('parked ok')
print('PASS')
