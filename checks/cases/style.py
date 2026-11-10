import os
import subprocess
import sys

from harness.api import check
from harness.env import SRCDIR

# checks/check-style, which holds the code to the style rules of
# CONTRIBUTING.md that no older code breaks, and the code it checks.

CHECKER = os.path.join(SRCDIR, 'checks', 'check-style')

GIT_ENV = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1',
               GIT_AUTHOR_NAME='A Person', GIT_AUTHOR_EMAIL='person@example.org',
               GIT_COMMITTER_NAME='A Person', GIT_COMMITTER_EMAIL='person@example.org')

# Files that keep the rules, among them some that only look as if they break
# one: in a comment, a literal, or a file the rules do not cover.
FOLLOW = {
    'common/ok.c': '#if defined(A) && !defined(B)\n#endif\n'
                   '// An enum, as defined (by the spec).\n'
                   '/* enum { A }; */\n'
                   'const char *s = "enum defined (A)";\n'
                   'char c = \'\\\'\';\n'
                   '#error Can\'t enum here\n',
    'common/lib/ok.h': '#ifndef LIB__OK_H\n#define LIB__OK_H\n\nint ok(void);\n\n#endif\n',
    'common/lib/licensed.h': '/*\n * A license.\n */\n\n#ifndef LIB__LICENSED_H\n'
                             '#define LIB__LICENSED_H\n\n#endif\n',
    'common/libc-compat/stdio.h': '#ifndef STDIO_H\n#define STDIO_H\n\n#endif\n',
    'checks/sim/stubs/sys/stub.h': '#ifndef SYS__STUB_H\n#define SYS__STUB_H\n\n#endif\n',
    'host/tool.h': '#ifndef TOOL_H\n#define TOOL_H\n\n#endif\n',
    'config.h.in': '#ifndef CONFIG_H\n#define CONFIG_H\n\n#define X "@X@"\n\n#endif\n',
    'common/lib/macros.aarch64_asm.h': '#ifndef LIB__MACROS_AARCH64_ASM_H\n'
                                       '#define LIB__MACROS_AARCH64_ASM_H\n\n'
                                       '.macro NOTHING\n.endm\n\n#endif\n',
    'common/nasm.asm_x86': '; As defined (by the spec).\n',
    'docs/NOTES.md': 'An enum is defined (here).\n'
}

# Files that break a rule, and the report each is expected to get.
BREAK = {
    'common/space.c': ('#if defined (A)\n#endif\n',
                       'common/space.c:1: `defined (`'),
    'common/continued.c': ('#if A \\\n || defined (B)\n#endif\n',
                           'common/continued.c:2: `defined (`'),
    'common/space.S': ('#if defined (A)\n#endif\n',
                       'common/space.S:1: `defined (`'),
    'common/linker.ld.in': ('#if defined (A)\n#endif\n',
                            'common/linker.ld.in:1: `defined (`'),
    'common/gas.asm_riscv64': ('#if defined (A)\n#endif\n',
                               'common/gas.asm_riscv64:1: `defined (`'),
    'common/anonymous.c': ('enum { A, B };\n',
                           'common/anonymous.c:1: an `enum`'),
    'common/typed.c': ('int a;\n\ntypedef enum {\n    A\n} e_t;\n',
                       'common/typed.c:3: an `enum`'),
    'common/lib/trailing.h': ('#ifndef LIB__TRAILING_H__\n#define LIB__TRAILING_H__\n\n#endif\n',
                              'common/lib/trailing.h: the include guard is not '
                              '`#ifndef LIB__TRAILING_H`'),
    'common/lib/misnamed.h': ('#ifndef LIB__OTHER_H\n#define LIB__OTHER_H\n\n#endif\n',
                              'common/lib/misnamed.h: the include guard is not '
                              '`#ifndef LIB__MISNAMED_H`'),
    'common/lib/valued.h': ('#ifndef LIB__VALUED_H\n#define LIB__VALUED_H 1\n\n#endif\n',
                            'common/lib/valued.h: the include guard is not'),
    'common/lib/late.h': ('int late;\n\n#ifndef LIB__LATE_H\n#define LIB__LATE_H\n\n#endif\n',
                          'common/lib/late.h: the include guard is not'),
    'common/lib/unguarded.h': ('int unguarded(void);\n',
                               'common/lib/unguarded.h: the include guard is not'),
    'common/lib/once.h': ('#pragma once\n\nint once(void);\n',
                          'common/lib/once.h: `#pragma once`'),
    'common/lib/named_end.h': ('#ifndef LIB__NAMED_END_H\n#define LIB__NAMED_END_H\n\n'
                               '#endif // LIB__NAMED_END_H\n',
                               'common/lib/named_end.h:4: the include guard does not end with '
                               'a bare `#endif`'),
    'test/test.h': ('#ifndef TEST__TEST_H\n#define TEST__TEST_H\n\n#endif\n',
                    'test/test.h: the include guard is not `#ifndef TEST_H`')
}


def run(t, *args):
    env = dict(os.environ)
    env.pop('GITHUB_ACTIONS', None)
    res = subprocess.run([sys.executable, CHECKER, *args], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    return res.returncode, res.stdout.decode()


def make_repo(t, files):
    """A git repository with files committed in it."""
    repo = t.path('repo')

    def git(*args):
        try:
            res = subprocess.run(['git', *args], cwd=repo, env=GIT_ENV, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        except OSError:
            t.skip('no git')
        t.require(res.returncode == 0, f'git {" ".join(args)} failed: {res.stdout.decode()}')

    for name, text in files.items():
        path = os.path.join(repo, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
    git('init', '-q')
    git('add', '.')
    git('commit', '-q', '-m', 'Add the files')
    return repo


@check(ports=['bios'], tags=['ci'])
def style_tree_follows(t):
    """Limine's own code keeps every rule the check holds it to."""
    res = subprocess.run(['git', '-C', SRCDIR, 'rev-parse', '--git-dir'], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    if res.returncode != 0:
        t.skip('the source tree is not a git checkout')
    code, out = run(t, SRCDIR)
    t.expect_eq(code, 0, f'the source tree passes: {out}')


@check(ports=['bios'], tags=['ci'])
def style_rules_hold(t):
    """Each file that breaks a rule is reported for it, at its line, while
    files that keep the rules, that the rules do not cover, or that git does
    not track are not, and each file is checked as it is on disk."""
    files = dict(FOLLOW, **{name: text for name, (text, _) in BREAK.items()})
    repo = make_repo(t, dict(files, **{'common/fixed.c': 'enum { A };\n'}))
    with open(os.path.join(repo, 'common', 'fixed.c'), 'w') as f:
        f.write('#define A 0\n')
    with open(os.path.join(repo, 'common', 'untracked.c'), 'w') as f:
        f.write('enum { B };\n')
    code, out = run(t, repo)
    t.expect_eq(code, 1, f'the files that break a rule fail: {out}')
    for name, (_, report) in BREAK.items():
        t.check(report in out, f'{name} is reported as {report!r}: {out}')
    for name in list(FOLLOW) + ['common/fixed.c', 'common/untracked.c']:
        t.check(name not in out, f'{name} is not reported: {out}')
    t.check(f'in {len(BREAK)} of ' in out, f'{len(BREAK)} files fail: {out}')
