#!/usr/bin/env python3
"""Scalarize the pinned vendor ISP binary; reject any unrecognized input/site.

No third-party Python dependencies. Requires the SDK binutils. This is a
binary-specific transformation, not a general RISC-V optimizer.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
from functools import lru_cache
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile

SOURCE_SHA256 = '92d9a7b548a1e1233a82f728ee8320f82778383ce7f4e580b9cf54a4966d62a3'
DEFAULT_PREFIX = '/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-'
VECTOR_COUNTS = {'vsetivli': 786, 'vmv.v.i': 96, 'vse8.v': 672, 'vmv.x.s': 18}
STORES = {'sb', 'sh', 'sw', 'sd', 'fsw', 'fsd'}
BRANCHES = {'beq', 'bne', 'blt', 'bge', 'bltu', 'bgeu', 'beqz', 'bnez', 'blez', 'bgez', 'bltz', 'bgtz', 'bgt', 'ble', 'bgtu', 'bleu', 'j'}
CALLS = {'jal', 'jalr', 'call'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, **kwargs)
    if result.returncode:
        detail = result.stderr.decode(errors='replace') if isinstance(result.stderr, bytes) else result.stderr
        raise ValueError(f'{args[0]} exited {result.returncode}: {detail}')
    return result.stdout


@dataclass(frozen=True)
class Instruction:
    address: int
    word: str
    mnemonic: str
    operands: str

    @property
    def size(self):
        return len(self.word) // 2


def disassemble(path, prefix):
    result = []
    for line in run(prefix + 'objdump', '-d', str(path), text=True).splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)(?:\s+(.*))?$', line)
        if match:
            address, word, mnemonic, operands = match.groups()
            result.append(Instruction(int(address, 16), word, mnemonic,
                                      (operands or '').split('#')[0].strip()))
    require(result, 'empty disassembly')
    return result


def text_section(blob):
    require(blob[:6] == b'\x7fELF\x02\x01', 'expected little-endian ELF64')
    shoff = struct.unpack_from('<Q', blob, 40)[0]
    entsize, count, strings = struct.unpack_from('<HHH', blob, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', blob, shoff + i * entsize)
               for i in range(count)]
    names = headers[strings]
    for header in headers:
        start = names[4] + header[0]
        if blob[start:blob.index(b'\0', start)] == b'.text':
            return header[3], header[4], header[5]
    raise ValueError('no .text')


def destination(ins):
    if ins.mnemonic in STORES | BRANCHES | CALLS | {'ret', 'nop', 'fence', 'fence.i'}:
        return None
    return ins.operands.split(',')[0]


def analyze(instructions, original):
    """Backward-slice stack destinations and straight-line vector zero state.

    Arithmetic definitions must be in straight-line code. Frame pointers are
    checked across predecessor edges, with a matching allocation and saved s0;
    no guessed initial s0 value. Calls preserve the ABI's s0/sp.
    """
    indexes = {ins.address: i for i, ins in enumerate(instructions)}
    targets = set()
    predecessors = {i: [] for i in range(len(instructions))}
    for i, ins in enumerate(instructions):
        if ins.mnemonic in BRANCHES | CALLS:
            match = re.search(r'(?:^|,)([0-9a-f]+)\s+<', ins.operands)
            if match:
                target = int(match.group(1), 16)
                targets.add(target)
                if ins.mnemonic in BRANCHES and target in indexes:
                    predecessors[indexes[target]].append(i)
        if i + 1 < len(instructions) and ins.mnemonic not in {'j', 'jr', 'ret'}:
            predecessors[i + 1].append(i)

    # Pinned CamEngineVideoInSetMcmWrCfg switch: unsigned index <= 8,
    # signed 32-bit offsets relative to 0x1c6ce8, dispatched by jr a5.
    switch = 0xaa338
    require(instructions[indexes[switch]].operands == 'a5', 'switch changed')
    for relative in struct.unpack_from('<9i', original, 0x1c6ce8):
        target = 0x1c6ce8 + relative
        require(target in {0xaa37a, 0xaa33a, 0xaa342, 0xaa34a, 0xaa352, 0xaa35a, 0xaa362, 0xaa36a, 0xaa372}, 'switch target changed')
        targets.add(target)
        predecessors[indexes[target]].append(indexes[switch])

    @lru_cache(None)
    def frame_pointer(index):
        pending = list(predecessors[index])
        seen = set()
        definitions = set()
        while pending:
            j = pending.pop()
            if j in seen:
                continue
            seen.add(j)
            ins = instructions[j]
            if destination(ins) == 's0':
                args = ins.operands.split(',')
                require(ins.mnemonic == 'addi' and args[:2] == ['s0', 'sp'],
                        f'unsupported reaching frame definition at {ins.address:x}')
                frame = int(args[2], 0)
                require(frame > 0 and frame % 16 == 0, f'bad frame at {ins.address:x}')
                prologue = instructions[max(0, j - 8):j]
                allocation = next((p for p in reversed(prologue)
                                   if p.mnemonic == 'addi' and p.operands == f'sp,sp,{-frame}'), None)
                require(allocation is not None, f'no stack allocation at {ins.address:x}')
                require(any(p.mnemonic == 'sd' and p.operands == f's0,{frame-16}(sp)'
                            for p in prologue if p.address > allocation.address),
                        f'no saved frame pointer at {ins.address:x}')
                require(all(p.mnemonic not in BRANCHES | CALLS and destination(p) != 'sp'
                            and p.address not in targets
                            for p in prologue if p.address > allocation.address), 'nonlinear stack prologue')
                definitions.add(j)
            else:
                require(predecessors[j], f'frame pointer can reach an unknown entry at {ins.address:x}')
                pending.extend(predecessors[j])
        require(len(definitions) == 1, 'frame pointer has ambiguous reaching definitions')
        return ('frame', 0)

    @lru_cache(None)
    def resolve(index, register):
        if register == 'zero':
            return ('constant', 0)
        if register == 's0':
            return frame_pointer(index)
        for j in range(index - 1, -1, -1):
            ins = instructions[j]
            op = ins.mnemonic
            args = ins.operands.split(',')
            require(op not in BRANCHES | {'ret', 'jr'},
                    f'control flow in address slice for {register} at {ins.address:x}')
            if op in CALLS:
                require(register in {'s0', 'sp'}, f'call clobbers {register} at {ins.address:x}')
            if destination(ins) == register:
                if op in {'lui', 'li'}:
                    value = int(args[1], 0)
                    if op == 'lui':
                        value <<= 12
                        if value & (1 << 31):
                            value -= 1 << 32
                    return ('constant', value)
                if op == 'mv':
                    return resolve(j, args[1])
                if op == 'addi':
                    kind, value = resolve(j, args[1])
                    return kind, value + int(args[2], 0)
                if op == 'add':
                    left, right = resolve(j, args[1]), resolve(j, args[2])
                    require(left[0] == 'constant' or right[0] == 'constant', 'adding two frame pointers')
                    return ('frame' if 'frame' in (left[0], right[0]) else 'constant', left[1] + right[1])
                raise ValueError(f'unsupported definition {ins} for {register}')
            require(ins.address not in targets, f'branch entry in address slice at {ins.address:x}')
        raise ValueError(f'no definition of {register}')

    vector = [ins for ins in instructions if ins.mnemonic.startswith('v')]
    require(Counter(ins.mnemonic for ins in vector) == VECTOR_COUNTS, 'unexpected RVV inventory')
    alignments = {}
    for ins in vector:
        require(ins.size == 4, f'non-32-bit RVV at {ins.address:x}')
        if ins.mnemonic == 'vsetivli':
            require(ins.operands in {'zero,8,e8,mf2,ta,ma', 'zero,0,e8,mf2,ta,ma',
                                    'zero,0,e16,mf2,ta,ma', 'zero,0,e32,mf2,ta,ma'}, 'unexpected vtype')
        elif ins.mnemonic == 'vmv.v.i':
            require(ins.operands == 'v1,0', 'nonzero vector initializer')
            require(ins.address not in targets, 'branch bypasses initializer configuration')
            previous = instructions[indexes[ins.address] - 1]
            require(previous.mnemonic == 'vsetivli' and previous.operands == 'zero,8,e8,mf2,ta,ma',
                    'initializer VL is not eight bytes')
        else:
            index = indexes[ins.address]
            require(ins.address not in targets, f'entry at vector consumer {ins.address:x}')
            for prev in reversed(instructions[:index]):
                require(prev.mnemonic not in BRANCHES | CALLS | {'ret', 'jr'}, 'zero provenance crosses control flow')
                if prev.mnemonic == 'vmv.v.i':
                    break
                require(prev.address not in targets, f'entry after vector initializer {prev.address:x}')
            else:
                raise ValueError('no vector zero initializer')
            if ins.mnemonic == 'vse8.v':
                require(ins.operands in {'v1,(a4)', 'v1,(a5)'}, 'unexpected vector store')
                prev = instructions[index - 1]
                require(prev.mnemonic == 'vsetivli' and prev.operands == 'zero,8,e8,mf2,ta,ma', 'store VL not eight')
                kind, offset = resolve(index, ins.operands[4:6])
                require(kind == 'frame', 'destination is not stack storage')
                alignments[ins.address] = offset
            else:
                require(ins.operands == 'a5,v1', 'unexpected vector extraction')
    require(Counter(value % 8 for value in alignments.values()) == {0: 667, 6: 2, 2: 2, 3: 1},
            f'unexpected alignment inventory: {Counter(value % 8 for value in alignments.values())}')
    return vector, alignments, targets


def assemble(operations, prefix, directory):
    """Use the real assembler, with compression disabled, instead of hand encoding."""
    obj = directory / 'replacements.o'
    raw = directory / 'replacements.bin'
    source = '.text\n.option norvc\n' + '\n'.join(operations) + '\n'
    run(prefix + 'as', '-march=rv64gc', '-o', str(obj), '-', input=source.encode())
    run(prefix + 'objcopy', '-O', 'binary', '-j', '.text', str(obj), str(raw))
    result = raw.read_bytes()
    require(len(result) == 4 * len(operations), 'assembler changed instruction widths')
    return [result[i:i+4] for i in range(0, len(result), 4)]


def patch(source, output, prefix):
    original = source.read_bytes()
    require(hashlib.sha256(original).hexdigest() == SOURCE_SHA256, 'unsupported input SHA-256')
    require(source.resolve() != output.resolve(), 'input and output must differ')
    all_instructions = disassemble(source, prefix)
    text_addr, text_off, text_size = text_section(original)
    instructions = [ins for ins in all_instructions if text_addr <= ins.address < text_addr + text_size]
    vector, alignments, targets = analyze(instructions, original)
    require(len(vector) == sum(ins.mnemonic.startswith('v') for ins in all_instructions), 'RVV outside .text')
    by_address = {ins.address: ins for ins in instructions}
    replacements = {}
    for ins in vector:
        if ins.mnemonic == 'vse8.v':
            replacements[ins.address] = f'sd zero,0({ins.operands[4:6]})'
        elif ins.mnemonic == 'vmv.x.s':
            replacements[ins.address] = 'addi a5,zero,0'
        else:
            replacements[ins.address] = 'nop'

    # Explicit nonoverlapping windows. Never overwrite the intervening addi.
    special = {
        0x147f46: ([0x147f3a, 0x147f3e, 0x147f46], [('sh', 0), ('sw', 2), ('sh', 6)]),
        0x147f52: ([0x147f4e, 0x147f52, 0x147f56], [('sh', 0), ('sw', 2), ('sh', 6)]),
        0x173106: ([0x1730fa, 0x1730fe, 0x173106], [('sh', 0), ('sw', 2), ('sh', 6)]),
        0x173112: ([0x17310e, 0x173112, 0x173116], [('sh', 0), ('sw', 2), ('sh', 6)]),
        0x1730ae: ([0x1730a2, 0x1730a6, 0x1730aa, 0x1730ae], [('sb', 0), ('sw', 1), ('sh', 5), ('sb', 7)]),
    }
    require(set(special) == {addr for addr, offset in alignments.items() if offset % 8}, 'special sites differ')
    occupied = set()
    for store, (slots, operations) in special.items():
        covered = []
        for addr, (mnemonic, offset) in zip(slots, operations):
            require(addr not in occupied and addr in replacements, 'overlapping or non-vector patch')
            occupied.add(addr)
            require(by_address[addr].mnemonic in {'vsetivli', 'vmv.v.i', 'vse8.v'}, 'overwrites scalar extraction')
            width = {'sb': 1, 'sh': 2, 'sw': 4}[mnemonic]
            require((alignments[store] + offset) % width == 0, 'misaligned scalar replacement')
            covered.extend(range(offset, offset + width))
            replacements[addr] = f'{mnemonic} zero,{offset}(a5)'
        require(sorted(covered) == list(range(8)), 'incorrect byte coverage')
        for ins in instructions:
            if slots[0] <= ins.address <= slots[-1]:
                require(ins.address not in targets, 'branch into expanded store')
                require(ins.mnemonic.startswith('v'), 'scalar instruction inside expansion')

    with tempfile.TemporaryDirectory(prefix='isp-scalar-') as directory_name:
        directory = Path(directory_name)
        addresses = sorted(replacements)
        encoded = assemble([replacements[a] for a in addresses], prefix, directory)
        blob = bytearray(original)
        permitted = set()
        for address, code in zip(addresses, encoded):
            offset = text_off + address - text_addr
            require(original[offset:offset+4] == int(by_address[address].word, 16).to_bytes(4, 'little'), 'original bytes differ')
            blob[offset:offset+4] = code
            permitted.update(range(offset, offset+4))
        require(len(blob) == len(original), 'length changed')
        require(all(a == b or i in permitted for i, (a, b) in enumerate(zip(original, blob))), 'unapproved byte change')
        candidate = directory / 'candidate'
        candidate.write_bytes(blob)
        after = disassemble(candidate, prefix)
        require(len(after) == len(all_instructions), 'instruction boundaries changed')
        for before, now in zip(all_instructions, after):
            require(before.address == now.address and before.size == now.size, 'instruction layout changed')
            require(not now.mnemonic.startswith('v'), 'remaining RVV')
            if before.address not in replacements:
                require(before == now, f'unexpected instruction change at {before.address:x}')
        output.write_bytes(blob)
        output.chmod(0o755)
    print(f'Verified: {len(vector)} RVV instructions replaced; 667 aligned and 5 expanded stores; no RVV remains.')
    print(f'SHA-256 {hashlib.sha256(blob).hexdigest()}  {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--tool-prefix', default=DEFAULT_PREFIX)
    args = parser.parse_args()
    try:
        patch(args.source, args.output, args.tool_prefix)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Verification failed: {error}\n')


if __name__ == '__main__':
    main()
