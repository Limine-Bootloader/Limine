import os
import subprocess
import sys

from harness.api import check
from harness.env import SRCDIR

# .github/scripts/check_commit_messages.py, which CI runs on the commits of a
# pull request, held to the "Commit conventions" of CONTRIBUTING.md.

CHECKER = os.path.join(SRCDIR, '.github', 'scripts', 'check_commit_messages.py')

TRAILER = '\n\nAssisted-by: Claude:claude-opus-5-5\n'

FOLLOW = [
    'lib/pe: Verify the image\'s hash before loading it\n',
    'lib/pe: Verify the image\'s hash before loading it' + TRAILER,
    'protos/linux: Stop claiming framebuffer quirks can be skipped\n\n'
    'The flag asserts screen_info was not filled from firmware data, so\n'
    'Linux skips its DMI framebuffer fixups. Ours comes from GOP.\n\n'
    'Assisted-by: Codex:gpt-6-astra\nSigned-off-by: A Person <person@example.org>\n',
    'sys/smp, sys/lapic: Enable AMD 8-bit APIC IDs\n\nAssisted-by: Antigravity:gemini-4-argon\n',
    'checks: Cover revoked certificates\n',
    'docs/CONFIG.md: Say that global options must precede the first entry\n',
    'build: bootstrap: Bump PicoEFI\n',
    'lib/gterm: Embed the default font\n',
    'lib/mem: Bring back the assembly memory routines\n',
    'stage1/{cd,hdd,pxe}: Load stage 2 above the EBDA\n',
    'ci: Ship 3RDPARTY.md in the binary release archives\n',
    'test: Use printf instead of the echo -e bashism\n',
    'Revert "lib/pe: Verify the image\'s hash before loading it"\n\n'
    'This reverts commit 0123456789abcdef0123456789abcdef01234567.\n',
    'misc: Drop the space between defined and its parenthesis' + TRAILER
    + 'Co-authored-by: Jane Doe <jane@example.org>\n'
    'Co-authored-by: Claude Dupont <claude@example.org>\n'
]

BREAK = [
    ('feat(lib/pe): verify the image\'s hash before loading it\n', 'Conventional Commit'),
    ('fix: protos/limine: read the memory map count after the allocation\n',
     '`fix` is a Conventional Commits type'),
    ('test(checks/secureboot): cover revoked certificates\n', 'Conventional Commit'),
    ('lib/pe!: Verify the image\'s hash before loading it\n', 'no `!`'),
    ('drivers/gop, checks: Fix the mode list\n', 'drop `checks`'),
    ('checks/secureboot: Cover revoked certificates\n', 'area is `checks`'),
    ('lib/pe: verify the image\'s hash before loading it\n', 'uppercase letter'),
    ('lib/pe: Verify the image\'s hash before loading it.\n', 'period'),
    ('lib/pe: Verified the image\'s hash before loading it\n', 'imperative'),
    ('lib/pe: Verifying the image\'s hash before loading it\n', 'imperative'),
    ('lib/pe: Verifies the image\'s hash before loading it\n', 'imperative'),
    ('Verify the image\'s hash before loading it\n', 'not `<area>: <Summary>`'),
    ('Lib/PE: Verify the image\'s hash before loading it\n', 'not lowercase'),
    ('lib/pe: Verify the image\'s hash before loading it\nIt was not.\n', 'no blank line'),
    ('lib/pe: Verify the image\'s hash\n\nOne.\nTwo.\nThree.\nFour.' + TRAILER,
     'the body is 4 lines long'),
    ('lib/pe: Verify the image\'s hash\n\nAssisted-by: Antigravity:gemini-4-argon-high\n',
     'reasoning effort'),
    ('lib/pe: Verify the image\'s hash\n\nAssisted-by: Claude:claude-opus-5[1m]\n',
     'exact identifier'),
    ('lib/pe: Verify the image\'s hash\n\nAssisted-by: Claude:Claude-Opus-5-5\n',
     'exact identifier'),
    ('lib/pe: Verify the image\'s hash\n\nAssisted-by: claude-opus-5-5\n',
     'AGENT_NAME:MODEL_VERSION'),
    ('lib/pe: Verify the image\'s hash\n\nAssisted-By: Claude:claude-opus-5-5\n',
     'spelt `Assisted-by:`'),
    ('lib/pe: Verify the image\'s hash' + TRAILER
     + 'Co-authored-by: Claude <noreply@anthropic.com>\n', 'credits an AI assistant'),
    ('lib/pe: Verify the image\'s hash' + TRAILER
     + 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n', 'credits an AI assistant'),
    ('lib/pe: Verify the image\'s hash\n\nCo-authored-by: copilot-swe-agent[bot] '
     '<198982749+Copilot@users.noreply.github.com>\n', 'credits an AI assistant'),
    ('lib/pe: Verify the image\'s hash\n\n'
     '\U0001f916 Generated with [Claude Code](https://claude.com/claude-code)\n',
     '"Generated with"'),
    ('lib/pe: Verify the image\'s hash — always\n', 'non-ASCII'),
    ('lib/pe: Verify the image\'s hash' + TRAILER
     + '\nSigned-off-by: A Person <person@example.org>\n', 'not in the last paragraph'),
    ('\n', 'empty')
]


def run(t, args, cwd=None):
    if not os.path.exists(CHECKER):
        t.skip('this tree has no .github/scripts/check_commit_messages.py')
    env = dict(os.environ)
    env.pop('GITHUB_ACTIONS', None)
    res = subprocess.run([sys.executable, CHECKER, *args], cwd=cwd, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return res.returncode, res.stdout.decode()


def run_message(t, name, text):
    path = t.path(name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    return run(t, ['--message-file', path])


def tracks_files():
    """Whether the source tree is a git checkout, whose files areas may quote."""
    res = subprocess.run(['git', '-C', SRCDIR, 'ls-files', 'ChangeLog'],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return res.returncode == 0 and res.stdout.strip() != b''


@check(ports=['bios'], tags=['ci'])
def commit_messages_follow(t):
    """Messages that keep CONTRIBUTING.md's rules, and its examples of them,
    pass, as do a long subject, which is only warned about, and a file name
    quoted in an area whatever its case."""
    follow = list(FOLLOW)
    if tracks_files():
        follow.append('docs/ChangeLog: Update for 12.9.6\n')
    for i, text in enumerate(follow):
        code, out = run_message(t, f'follow{i}.txt', text)
        t.expect_eq(code, 0, f'{text.splitlines()[0]!r} passes: {out}')
    code, out = run_message(t, 'long.txt', 'drivers/gop: Pick the mode closest to the preferred '
                            'resolution when none of them matches it\n')
    t.expect_eq(code, 0, f'a long subject passes: {out}')
    t.check('warning: the subject is 91 columns long' in out,
            f'a long subject is warned about: {out}')


@check(ports=['bios'], tags=['ci'])
def commit_messages_break(t):
    """Each way of breaking a rule fails the check, and its report says which
    rule it broke."""
    for i, (text, why) in enumerate(BREAK):
        code, out = run_message(t, f'break{i}.txt', text)
        t.expect_eq(code, 1, f'{text!r} fails')
        t.check(why in out, f'{text!r} fails for {why!r}: {out}')


@check(ports=['bios'], tags=['ci'])
def commit_messages_range(t):
    """A range is checked commit by commit, merges left out, as CI checks a
    pull request."""
    repo = t.path('repo')
    os.makedirs(repo)
    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1',
               GIT_AUTHOR_NAME='A Person', GIT_AUTHOR_EMAIL='person@example.org',
               GIT_COMMITTER_NAME='A Person', GIT_COMMITTER_EMAIL='person@example.org')

    def git(*args):
        res = subprocess.run(['git', *args], cwd=repo, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        t.require(res.returncode == 0, f'git {" ".join(args)} failed: {res.stdout.decode()}')
        return res.stdout.decode().strip()

    try:
        git('init', '-q', '-b', 'trunk')
    except OSError:
        t.skip('no git')
    git('commit', '-q', '--allow-empty', '-m', 'lib/pe: Verify the image\'s hash')
    base = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'lib/elf: Reject empty load images')
    good = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'feat(lib/elf): reject empty load images')
    bad = git('rev-parse', 'HEAD')
    git('checkout', '-q', '-b', 'side', base)
    git('commit', '-q', '--allow-empty', '-m', 'lib/acpi: Check the checksum of the RSDP')
    git('checkout', '-q', 'trunk')
    git('merge', '-q', '--no-ff', '--no-edit', 'side')

    code, out = run(t, ['--range', f'{base}..{good}'], cwd=repo)
    t.expect_eq(code, 0, f'a range of good commits passes: {out}')
    code, out = run(t, ['--range', f'{base}..HEAD'], cwd=repo)
    t.expect_eq(code, 1, f'a range with a bad commit fails: {out}')
    t.check(f'{bad[:12]}: feat(lib/elf)' in out, f'the bad commit is reported: {out}')
    t.check(good[:12] not in out, f'the good commits are not reported: {out}')
    t.check('Merge branch' not in out, f'the merge is left out: {out}')
    t.check('1 of 3 commit messages' in out, f'three commits are checked: {out}')
