#!/usr/bin/env python3
"""Execute actual patched store windows under scalar QEMU with memory guards.

Usage: python3 test_devectorize_isp.py /tmp/isp_media_server_scalar_candidate
The candidate must first pass devectorize_isp.py's static verification.
"""
import argparse
from pathlib import Path
import tempfile

from devectorize_isp import DEFAULT_PREFIX, patch, require, run, text_section


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--tool-prefix', default=DEFAULT_PREFIX)
    parser.add_argument('--qemu', default='qemu-riscv64-static')
    args = parser.parse_args()
    blob = args.candidate.read_bytes()
    address, offset, _ = text_section(blob)
    # Include both adjacent expanded stores and their intervening scalar addi.
    # Final vmv.x.s replacement must leave a5 zero as the original did.
    cases = [
        (0x147f36, 0x147f5e, -42, 16),
        (0x17309e, 0x1730ba, -437, 8),
        (0x1730f6, 0x17311e, -382, 16),
        (0x7f1f2, 0x7f212, -56, 16),
    ]
    with tempfile.TemporaryDirectory(prefix='isp-scalar-exec-test-') as directory:
        directory = Path(directory)
        untouched = directory / 'existing-output'
        untouched.write_bytes(b'existing output must survive failed verification')
        for source, prefix in [(args.candidate, args.tool_prefix),
                               (Path(__file__).with_name('isp_media_server'), '/nonexistent/binutils-')]:
            try:
                patch(source, untouched, prefix)
            except (ValueError, OSError):
                pass
            else:
                raise AssertionError('invalid input/toolchain unexpectedly accepted')
            require(untouched.read_bytes() == b'existing output must survive failed verification',
                    'failed verification overwrote existing output')
        assembly = ['.text', '.option norvc', '.global _start', '_start:',
                    'addi sp,sp,-1024', 'addi s0,sp,512']
        for number, (start, end, target, count) in enumerate(cases):
            code = blob[offset+start-address:offset+end-address]
            assembly += ['mv t0,sp', 'li t1,1024', 'li t2,165', f'fill_{number}:',
                         'sb t2,0(t0)', 'addi t0,t0,1', 'addi t1,t1,-1', f'bnez t1,fill_{number}',
                         'li a4,85', 'li a5,102',
                         '.byte ' + ','.join(str(byte) for byte in code)]
            if number < 3:
                assembly += ['bnez a5,fail', 'li t0,85', 'bne a4,t0,fail']
            else:
                assembly += ['addi t0,s0,-48', 'bne a5,t0,fail', 'li t0,85', 'bne a4,t0,fail']
            # Check every byte, including guards around and between regions.
            assembly += ['li t0,0', 'mv t1,sp', f'check_{number}:', 'li t2,165',
                         f'li t3,{512+target}', f'blt t0,t3,compare_{number}',
                         f'li t3,{512+target+count}', f'bge t0,t3,compare_{number}',
                         'li t2,0', f'compare_{number}:', 'lbu t3,0(t1)', 'bne t2,t3,fail',
                         'addi t0,t0,1', 'addi t1,t1,1', 'li t3,1024', f'blt t0,t3,check_{number}']
        assembly += ['li a0,0', 'j exit', 'fail:', 'li a0,1', 'exit:', 'li a7,93', 'ecall']
        obj = directory / 'test.o'
        executable = directory / 'test'
        run(args.tool_prefix+'as', '-march=rv64gc', '-o', str(obj), '-',
            input=('\n'.join(assembly)+'\n').encode())
        run(args.tool_prefix+'ld', '-o', str(executable), str(obj))
        run(args.qemu, '-cpu', 'sifive-u54', str(executable))
    print('PASS: four actual scalar windows; exact zero coverage, guard bytes, and a4/a5 effects.')
    print('PASS: unexpected input and missing binutils fail without overwriting existing output.')


if __name__ == '__main__':
    main()
