import bisect
import json
import os
import re
import shutil
import struct
import subprocess

from harness.env import which
from harness.ports import PORTS

# Code compiled into Limine that the project takes from elsewhere; reported,
# but apart from Limine's own.
VENDORED = ('flanterm/', 'libfdt/', 'picoefi/', 'common/lib/stb_image.h', 'common/compress/pdgzip',
            'common/freestanding-lgrt', 'freestanding-c-hdrs/')
COVERAGE_FILE = 'coverage.bin'
# QEMU's log of each block of guest code it translates: what Limine ran,
# found by matching the instructions' encodings against its image.
TCG_LOG = 'tcg.log'
_X86_INSN = re.compile(rb'^0x([0-9a-f]+):  ((?:[0-9a-f]{2} )+)')
_WORD_INSN = re.compile(rb'^0x([0-9a-f]+):  ([0-9a-f]{8}|[0-9a-f]{4}) ')
MAX_BASES = 8
MAX_COPIES = 64
# Code Limine runs from copies it makes: the SMP trampolines, the 32-bit
# handoff code UEFI x86-64 Limine moves below 4GiB, and the FreeBSD hand-off,
# which moves itself out of the way of the blob it starts.
COPIED_SOURCES = ('smp_trampoline.', 'spinup.', 'spinup_freebsd.', 'limine_32.', 'linux_32.', 'multiboot_32.')


def tcg_blocks(path, x86):
    """The translated blocks of a QEMU -d in_asm log, as lists of (address,
    encoding): x86 prints an instruction's bytes, the others its instruction
    word, which is stored little-endian."""
    block = []
    pattern = _X86_INSN if x86 else _WORD_INSN
    with open(path, 'rb') as f:
        for line in f:
            if line.startswith(b'0x'):
                m = pattern.match(line)
                if m is None:
                    continue
                if x86:
                    code = bytes.fromhex(m.group(2).decode())
                else:
                    code = int(m.group(2), 16).to_bytes(len(m.group(2)) // 2, 'little')
                block.append((int(m.group(1), 16), code))
            elif line.startswith(b'IN:') and block:
                yield block
                block = []
    if block:
        yield block


def pe_entry(path):
    with open(path, 'rb') as f:
        data = f.read(4096)
    lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    return struct.unpack_from('<I', data, lfanew + 40)[0]


class Coverage:
    def __init__(self, env):
        self.env = env
        self.blobs = {name: self._blobs(name) for name in PORTS if os.path.exists(env.limine_elf(name))}
        self._images = {}
        self.host_tool = self._build_host_tool()
        env.host_tool = self.host_tool
        self.host_profiles = os.path.join(env.outdir, 'host-profiles')
        shutil.rmtree(self.host_profiles, ignore_errors=True)
        os.makedirs(self.host_profiles)

    @property
    def host_profile_pattern(self):
        """LLVM_PROFILE_FILE for the host tool's runs: merged as they exit into
        a pool of four files, so concurrent runs add up."""
        return os.path.join(self.host_profiles, 'limine-%4m.profraw')

    def _build_host_tool(self):
        # The build's copy, with the version filled in, beside the header of
        # the BIOS stages it embeds, and with the build's defines.
        src = self.env.bin('limine.c')
        built = os.path.join(self.env.bindir, 'limine')
        out = self.env.payload('tools', 'limine-cov')
        deps = [d for d in (src, built, self.env.bin('limine-bios-hdd.h')) if os.path.exists(d)]
        if os.path.exists(out) and all(os.path.getmtime(out) >= os.path.getmtime(d) for d in deps):
            return out
        os.makedirs(os.path.dirname(out), exist_ok=True)
        defines = [] if os.path.exists(self.env.bin('limine-bios-hdd.h')) else ['-DLIMINE_NO_BIOS']
        res = subprocess.run([built, '--print-datadir'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if res.returncode == 0:
            defines.append(f'-DLIMINE_DATADIR="{res.stdout.decode().strip()}"')
        subprocess.run([which('clang'), '-std=c99', '-D_FILE_OFFSET_BITS=64', *defines, '-O0', '-g',
                        '-fprofile-instr-generate', '-fcoverage-mapping', src, '-o', out], check=True)
        return out

    def _host_lines(self):
        """{line: count} of the host tool's source, or None if it never ran."""
        profiles = [os.path.join(self.host_profiles, f) for f in os.listdir(self.host_profiles)
                    if f.endswith('.profraw')]
        if not profiles:
            return None
        data = os.path.join(self.host_profiles, 'limine.profdata')
        subprocess.run([which('llvm-profdata'), 'merge', '-sparse', '-o', data, *profiles], check=True)
        out = subprocess.run([which('llvm-cov'), 'export', '-format=lcov', f'-instr-profile={data}',
                              self.host_tool], stdout=subprocess.PIPE, check=True).stdout.decode()
        lines, ours = {}, False
        for rec in out.split('\n'):
            if rec.startswith('SF:'):
                ours = os.path.basename(rec[3:]) == 'limine.c'
            elif rec.startswith('DA:') and ours:
                n, count = rec[3:].split(',')[:2]
                lines[int(n)] = int(count)
        return lines

    def _image(self, port):
        if port.name == 'bios':
            return self.env.bin('limine-bios.sys'), 'base', 0xf000
        efi = self.env.bin(port.efi_name)
        return efi, 'entry', pe_entry(efi)

    def qemu_args(self, port, workdir):
        """Each machine has a directory of its own, where its log goes."""
        return ['-d', 'in_asm', '-D', os.path.join(workdir, TCG_LOG)]

    def collect(self, workdir):
        """Turn the translation log of each machine of a check into its record
        of the image offsets run, and drop the log."""
        port_name = os.path.basename(workdir)
        for root, _, files in os.walk(workdir):
            if TCG_LOG in files:
                log = os.path.join(root, TCG_LOG)
                offsets = sorted(self._offsets(PORTS[port_name], log))
                with open(os.path.join(root, COVERAGE_FILE), 'wb') as f:
                    f.write(struct.pack(f'<{len(offsets)}I', *offsets))
                os.unlink(log)

    def _offsets(self, port, log):
        """The image offsets of the instructions a log shows run. At a base
        from fixed (BIOS) or found where a block at the entry point matches
        the image (UEFI, where firmware may load it more than once), an
        instruction counts only if its encoding is the image's there. Code the
        image copies elsewhere counts where its bytes are found in the copied
        ranges: once a block matches in one place only, eight bytes at least,
        where that copy runs is known, and blocks too short or too common to
        place alone count for their place in a copy so known, at the end if
        none was by then."""
        path, kind, value = self._image(port)
        if path not in self._images:
            with open(path, 'rb') as f:
                self._images[path] = f.read()
        image = self._images[path]
        blobs = self.blobs.get(port.name, ())
        bases = [value] if kind == 'base' else []
        copies, pending, hit = [], [], set()

        def matches(base, addr, code):
            off = addr - base
            return 0 <= off and off + len(code) <= len(image) and image[off:off + len(code)] == code

        def in_blob(off, size):
            return any(b_off <= off < b_off + b_len and size <= b_len - (off - b_off) for b_off, b_len in blobs)

        def in_copy(start, code):
            found = [start - c for c in copies
                     if start >= c and in_blob(start - c, len(code)) and image[start - c:start - c + len(code)] == code]
            return found[0] if len(found) == 1 else None

        for block in tcg_blocks(log, port.is_x86):
            start = block[0][0]
            if kind == 'entry' and start >= value and len(bases) < MAX_BASES:
                candidate = start - value
                if candidate not in bases and candidate & 0xfff == 0 and matches(candidate, *block[0]):
                    bases.append(candidate)
            offsets = None
            for base in bases:
                offsets = [addr - base for addr, code in block if matches(base, addr, code)] or None
                if offsets:
                    break
            if offsets is None and blobs:
                end = block[-1][0] + len(block[-1][1])
                if end - start > 4096:
                    continue
                code = bytearray(end - start)
                for addr, insn in block:
                    code[addr - start:addr - start + len(insn)] = insn
                code = bytes(code)
                rel = [addr - start for addr, _ in block]
                off = in_copy(start, code)
                if off is None:
                    found = []
                    for b_off, b_len in blobs:
                        hay = image[b_off:b_off + b_len]
                        at = hay.find(code)
                        while at >= 0 and len(found) < 2:
                            found.append(b_off + at)
                            at = hay.find(code, at + 1)
                        if len(found) >= 2:
                            break
                    if len(found) == 1 and len(code) >= 8:
                        off = found[0]
                        if start - off not in copies and len(copies) < MAX_COPIES:
                            copies.append(start - off)
                    elif found:
                        pending.append((start, code, rel))
                if off is not None:
                    offsets = [off + r for r in rel]
            if offsets:
                hit.update(offsets)
        for start, code, rel in pending:
            off = in_copy(start, code)
            if off is not None:
                hit.update(off + r for r in rel)
        return hit

    @staticmethod
    def _base(port_name):
        return 0xf000 if port_name == 'bios' else 0

    def _line_table(self, port_name):
        """(address, size, path, line, function) of each instruction of a
        port's Limine, cached by the identity of its ELF."""
        elf = self.env.limine_elf(port_name)
        st = os.stat(elf)
        key = f'{st.st_mtime_ns}:{st.st_size}'
        cache = self.env.payload('tools', f'cov-lines-{port_name}.json')
        try:
            with open(cache) as f:
                data = json.load(f)
            if data['key'] == key:
                return [tuple(v) for v in data['table']]
        except (OSError, ValueError, KeyError):
            pass
        addrs = sorted(self._instructions(elf))
        locs = self._symbolize(elf, addrs)
        ends = addrs[1:] + [addrs[-1] + 16] if addrs else []
        table = [(a, end - a) + locs[a] for a, end in zip(addrs, ends) if a in locs]
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, 'w') as f:
            json.dump({'key': key, 'table': table}, f)
        return table

    def _blobs(self, port_name):
        """(offset, size) in the port's image of code run from copies."""
        base = self._base(port_name)
        limit = os.path.getsize(self._image(PORTS[port_name])[0])
        ranges = []
        for addr, size, path, _, _ in self._line_table(port_name):
            if not os.path.basename(path).startswith(COPIED_SOURCES):
                continue
            lo, hi = addr - base, min(addr - base + size, limit)
            if lo < 0 or lo >= hi:
                continue
            if ranges and lo - ranges[-1][1] <= 64:
                ranges[-1][1] = max(ranges[-1][1], hi)
            else:
                ranges.append([lo, hi])
        return [(lo, hi - lo) for lo, hi in ranges]

    @staticmethod
    def _records(workdirs):
        """(port, record) for each machine run in the given check directories,
        which are named after their port."""
        for workdir in workdirs:
            port_name = os.path.basename(workdir)
            for root, _, files in os.walk(workdir):
                if COVERAGE_FILE in files:
                    yield port_name, os.path.join(root, COVERAGE_FILE)

    def _executed(self, records):
        offsets = set()
        for path in records:
            with open(path, 'rb') as f:
                data = f.read()
            offsets.update(struct.unpack(f'<{len(data) // 4}I', data))
        return offsets

    def _instructions(self, elf):
        out = subprocess.run([which('llvm-objdump'), '-d', '--no-show-raw-insn', elf],
                             stdout=subprocess.PIPE, check=True).stdout.decode(errors='replace')
        addrs = []
        for line in out.split('\n'):
            m = re.match(r'^\s+([0-9a-f]+):\s', line)
            if m:
                addrs.append(int(m.group(1), 16))
        return addrs

    def _symbolize(self, elf, addrs):
        inp = ''.join(f'0x{a:x}\n' for a in addrs).encode()
        out = subprocess.run([which('llvm-symbolizer'), f'--obj={elf}', '--output-style=GNU',
                              '--functions=linkage', '--no-inlines'],
                             input=inp, stdout=subprocess.PIPE, check=True).stdout.decode(errors='replace')
        lines = out.split('\n')
        result, paths = {}, {}
        for i, a in enumerate(addrs):
            func = lines[2 * i].strip() if 2 * i < len(lines) else '??'
            loc = lines[2 * i + 1].strip() if 2 * i + 1 < len(lines) else '??:0'
            path, _, rest = loc.rpartition(':')
            m = re.match(r'(\d+)', rest)
            line = int(m.group(1)) if m else 0
            if path in ('', '??') or line == 0:
                continue
            if path not in paths:
                paths[path] = self._source_path(path)
            result[a] = (paths[path], line, func)
        return result

    def _source_path(self, path):
        """A path from the debug information, relative to the source tree.
        NASM's name the file relative to a directory other than the one
        recorded beside it, so leading components may have to go."""
        path = os.path.normpath(path)
        if path.startswith(self.env.srcdir + os.sep):
            path = path[len(self.env.srcdir) + 1:]
        parts = path.split(os.sep)
        for i in range(len(parts)):
            if os.path.exists(os.path.join(self.env.srcdir, *parts[i:])):
                return os.path.join(*parts[i:])
        return path

    def report(self, outdir, workdirs):
        """Report what the machines run in the check directories executed."""
        os.makedirs(outdir, exist_ok=True)
        records = {}
        for port_name, path in self._records(workdirs):
            records.setdefault(port_name, []).append(path)
        summary = {}
        union_all, union_hit = {}, {}
        func_seen, func_hit = {}, set()
        for port_name in sorted(records):
            base = self._base(port_name)
            table = self._line_table(port_name)
            # An executed offset counts for the instruction that holds it, as
            # the disassembly may split 16-bit code otherwise than it ran.
            starts = [a for a, *_ in table]
            hit = set()
            for o in self._executed(records[port_name]):
                i = bisect.bisect_right(starts, base + o) - 1
                if i >= 0 and base + o < starts[i] + table[i][1]:
                    hit.add(i)
            all_lines, hit_lines = {}, {}
            for i, (a, _, path, line, func) in enumerate(table):
                all_lines.setdefault(path, set()).add(line)
                # Assembler-local labels, such as RISC-V's .Lpcrel_hi*, are
                # no functions.
                if not func.startswith('.L'):
                    func_seen.setdefault((path, func), set()).add(port_name)
                if i in hit:
                    hit_lines.setdefault(path, set()).add(line)
                    if not func.startswith('.L'):
                        func_hit.add((path, func))
            for path, lines in all_lines.items():
                union_all.setdefault(path, set()).update(lines)
                union_hit.setdefault(path, set()).update(hit_lines.get(path, set()))
            own = [p for p in all_lines if not p.startswith(VENDORED)]
            total = sum(len(all_lines[p]) for p in own)
            hit = sum(len(hit_lines.get(p, ())) for p in own)
            summary[port_name] = {'lines': total, 'covered': hit,
                                  'percent': round(100.0 * hit / total, 2) if total else 0.0}
        host = self._host_lines()
        if host is not None:
            path = os.path.join('host', 'limine.c')
            union_all[path] = set(host)
            union_hit[path] = {n for n, count in host.items() if count}
            total, hit = len(union_all[path]), len(union_hit[path])
            summary['host'] = {'lines': total, 'covered': hit,
                               'percent': round(100.0 * hit / total, 2) if total else 0.0}
        own = sorted(p for p in union_all if not p.startswith(VENDORED))
        rows = []
        for p in own:
            total, hit = len(union_all[p]), len(union_hit[p])
            rows.append((p, total, hit))
        total = sum(r[1] for r in rows)
        hit = sum(r[2] for r in rows)
        summary['all'] = {'lines': total, 'covered': hit,
                          'percent': round(100.0 * hit / total, 2) if total else 0.0}
        with open(os.path.join(outdir, 'summary.json'), 'w') as f:
            json.dump(summary, f, indent=1)
        with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
            for k, v in summary.items():
                f.write(f'{k:18} {v["covered"]:6}/{v["lines"]:6} lines  {v["percent"]:6.2f}%\n')
            f.write('\nper file (all ports), least covered first:\n')
            for p, t, h in sorted(rows, key=lambda r: (r[2] / r[1] if r[1] else 1, r[0])):
                f.write(f'{100.0 * h / t if t else 100:6.1f}% {h:5}/{t:5}  {p}\n')
        with open(os.path.join(outdir, 'unexecuted-functions.txt'), 'w') as f:
            for (path, func), ports in sorted(func_seen.items()):
                if (path, func) not in func_hit and not path.startswith(VENDORED):
                    f.write(f'{path}: {func} [{", ".join(sorted(ports))}]\n')
        for p in own:
            src = os.path.join(self.env.srcdir, p)
            if not os.path.exists(src):
                continue
            dest = os.path.join(outdir, 'files', p + '.cov')
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(src, errors='replace') as fin, open(dest, 'w') as fout:
                for n, text in enumerate(fin, 1):
                    mark = ' '
                    if n in union_all[p]:
                        mark = '+' if n in union_hit[p] else '-'
                    fout.write(f'{mark} {n:5} {text}')
        host_note = ''
        if 'host' in summary:
            host_note = f', the host tool\'s {summary["host"]["covered"]}/{summary["host"]["lines"]} included'
        print(f'coverage: {summary["all"]["covered"]}/{summary["all"]["lines"]} lines '
              f'({summary["all"]["percent"]}%) of Limine\'s own code{host_note}; report in {outdir}')
