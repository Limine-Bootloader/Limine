import re


class Report:
    """What a check payload printed: "K key=value" facts, "C PASS|FAIL" checks,
    and the "E DONE" trailer."""

    def __init__(self, text, begin='B LIMINE-TEST'):
        self.raw = text
        start = text.rfind(begin)
        body = text[start:] if start >= 0 else text
        self.facts = {}
        self.multi = {}
        self.passes = []
        self.failures = []
        self.done = None
        for line in body.replace('\r', '').split('\n'):
            if line.startswith('K '):
                key, _, value = line[2:].partition('=')
                self.facts[key] = value
                self.multi.setdefault(key, []).append(value)
            elif line.startswith('C PASS '):
                self.passes.append(line[7:])
            elif line.startswith('C FAIL '):
                self.failures.append(line[7:])
            elif line.startswith('E DONE'):
                self.done = line
        self.header = body.split('\n', 1)[0] if start >= 0 else None

    def __getitem__(self, key):
        if key not in self.facts:
            raise KeyError(f'payload did not report {key!r}')
        return self.facts[key]

    def get(self, key, default=None):
        return self.facts.get(key, default)

    def __contains__(self, key):
        return key in self.facts

    def int(self, key, base=10):
        return int(self[key], base)

    def hex(self, key):
        return int(self[key], 16)

    def flag(self, key):
        return self[key] == '1'

    def prefixed(self, prefix):
        """Facts whose key starts with prefix, in report order."""
        return {k: v for k, v in self.facts.items() if k.startswith(prefix)}

    def indexed(self, prefix):
        """Values of prefix.N keys, ordered by N."""
        out = {}
        pat = re.compile(re.escape(prefix) + r'\.(\d+)$')
        for k, v in self.facts.items():
            m = pat.match(k)
            if m:
                out[int(m.group(1))] = v
        return [out[i] for i in sorted(out)]


def unescape(s):
    """Undo the payloads' \\xNN and \\uNNNN escaping."""
    s = re.sub(r'\\u([0-9a-f]{4})', lambda m: chr(int(m.group(1), 16)), s)
    return re.sub(r'\\x([0-9a-f]{2})', lambda m: chr(int(m.group(1), 16)), s)


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return f'{h:016x}'
