#!/usr/bin/env python3
"""Check commit messages against the "Commit conventions" of CONTRIBUTING.md.

--range BASE..HEAD checks each commit in that range but for merges, as CI
does for a pull request, and --message-file FILE checks the one message in
FILE. The exit status is 1 when a message breaks a rule.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Conventional Commits types that are no area here, as build, ci, docs, and test are.
COMMIT_TYPES = {
    'chore', 'feat', 'feature', 'fix', 'hotfix', 'perf', 'refactor', 'revert', 'style', 'tests',
    'wip'
}

# Verbs whose imperative ends in -ed or -ing, as their other forms do.
IMPERATIVE_ED_ING = {
    'bleed', 'breed', 'bring', 'cling', 'embed', 'exceed', 'feed', 'fling', 'heed', 'need',
    'ping', 'proceed', 'reseed', 'ring', 'seed', 'shed', 'shred', 'sing', 'sling', 'speed',
    'spring', 'sting', 'string', 'succeed', 'swing', 'wed', 'weed', 'wring'
}

# Forms of a verb other than its imperative that end in neither -ed nor -ing.
NOT_IMPERATIVE = {
    'Adds', 'Allows', 'Avoids', 'Brought', 'Built', 'Bumps', 'Changes', 'Checks', 'Cleans',
    'Converts', 'Corrects', 'Creates', 'Deletes', 'Disables', 'Drops', 'Enables', 'Ensures',
    'Fixes', 'Handles', 'Implements', 'Improves', 'Introduces', 'Kept', 'Made', 'Makes', 'Moves',
    'Prevents', 'Rebuilt', 'Refactors', 'Removes', 'Renames', 'Replaces', 'Reverts', 'Rewrote',
    'Supports', 'Updates', 'Uses', 'Validates', 'Verifies', 'Wrote'
}

AREA = re.compile(r'[A-Za-z0-9_.*+{},/-]+')
SCOPED_TYPE = re.compile(r'[A-Za-z]+\([^)]*\)!?:')
BANG_TYPE = re.compile(r'[A-Za-z0-9_./-]+!:')
REVERT = re.compile(r'(Revert|Reapply) ".+"')
TRAILER = re.compile(r'([A-Za-z][A-Za-z0-9-]*): (\S.*)')
ASSISTED = re.compile(r'(?P<agent>[A-Za-z0-9](?:[A-Za-z0-9 ._+-]*[A-Za-z0-9])?):(?P<model>\S+)')
MODEL = re.compile(r'[a-z0-9]+(?:[._-][a-z0-9]+)*')
EFFORT = re.compile(r'-(minimal|low|medium|high|xhigh)$')
GENERATED = re.compile(r'(?i)\bgenerated (with|by)\b.*\b(ai|antigravity|chatgpt|claude|codex'
                       r'|copilot|cursor|gemini|windsurf)\b')

# How AI assistants name themselves as co-authors, where people do not.
AI_DOMAINS = ('anthropic.com', 'cursor.com', 'openai.com')
AI_FIRST_WORDS = {'antigravity', 'chatgpt', 'codex', 'copilot', 'cursor', 'gemini', 'windsurf'}
AI_BOT = re.compile(r'(?i)(claude|codex|copilot|cursor|devin|gemini|jules|openhands)[\w-]*\[bot\]')
CLAUDE = re.compile(r'(?i)claude(?: (?:code|fable|haiku|opus|sonnet)\b.*)?')

WIDTH = 72


def tracked_names():
    """The names of the files git tracks here, which an area may quote."""
    try:
        out = subprocess.run(['git', '-C', ROOT, 'ls-files'], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, check=True).stdout.decode()
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {os.path.basename(path) for path in out.split('\n') if path != ''}


def parse(text):
    """A message's lines, its body lines, and its trailers as (key, value)."""
    lines = [line.rstrip() for line in text.strip('\n').split('\n')]
    rest = '\n'.join(lines[1:]).strip('\n')
    paragraphs = [p.split('\n') for p in re.split(r'\n\n+', rest) if p != '']
    trailers = []
    if paragraphs and all(TRAILER.fullmatch(line) for line in paragraphs[-1]):
        trailers = [TRAILER.fullmatch(line).groups() for line in paragraphs.pop()]
    body = [line for p in paragraphs for line in p]
    return lines, body, trailers


def credits_ai(author):
    """Whether a `Name <email>` names an AI assistant."""
    name, _, email = author.partition('<')
    domain = email.rstrip('>').strip().lower().rpartition('@')[2]
    return (AI_BOT.search(author) is not None
            or any(domain == d or domain.endswith('.' + d) for d in AI_DOMAINS)
            or any(word in AI_FIRST_WORDS for word in name.lower().split()[:1])
            or CLAUDE.fullmatch(name.strip()) is not None)


def check_area(area, names, errors):
    segments = area.split('/')
    for i, segment in enumerate(segments):
        if segment == segment.lower():
            continue
        if i == len(segments) - 1 and ('.' in segment or segment in names):
            continue
        errors.append(f'the area `{area}` is not lowercase, and only a file name it quotes may be')
        return


def check_subject(subject, names, errors, warnings):
    if len(subject) > WIDTH:
        warnings.append(f'the subject is {len(subject)} columns long, past about {WIDTH}')
    if REVERT.fullmatch(subject):
        return
    if SCOPED_TYPE.match(subject):
        errors.append('the subject is a Conventional Commit: no `type(scope):` prefix')
        return
    if BANG_TYPE.match(subject):
        errors.append('the subject is a Conventional Commit: no `!` before its colon')
        return

    levels = []
    rest = subject
    while True:
        head, sep, tail = rest.partition(': ')
        if sep == '' or not all(AREA.fullmatch(area) for area in head.split(', ')):
            break
        levels.append(head.split(', '))
        rest = tail
    if not levels:
        errors.append('the subject is not `<area>: <Summary>`')
        return

    for areas in levels:
        for area in areas:
            if area.lower() in COMMIT_TYPES:
                errors.append(f'`{area}` is a Conventional Commits type, not an area')
            elif area == 'checks' and len(areas) > 1:
                errors.append('the area names the checks that come with a change: drop `checks`')
            elif area.startswith('checks/'):
                errors.append(f'the test suite\'s area is `checks`, not `{area}`')
            else:
                check_area(area, names, errors)

    summary = rest
    if summary == '':
        errors.append('the summary is empty')
        return
    if not 'A' <= summary[0] <= 'Z':
        errors.append('the summary does not begin with an uppercase letter')
    if summary.endswith('.'):
        errors.append('the summary ends with a period')
    word = re.match(r'[A-Za-z]*', summary).group()
    if word.lower().endswith(('ed', 'ing')):
        imperative = word.lower() in IMPERATIVE_ED_ING
    else:
        imperative = word not in NOT_IMPERATIVE
    if not imperative:
        errors.append(f'the summary is not in the imperative mood: `{word}`')


def check_trailers(trailers, errors):
    for key, value in trailers:
        if key.lower() == 'assisted-by':
            if key != 'Assisted-by':
                errors.append(f'`{key}:` is spelt `Assisted-by:`')
            m = ASSISTED.fullmatch(value)
            if m is None:
                errors.append(f'`Assisted-by: {value}` is not '
                              '`Assisted-by: AGENT_NAME:MODEL_VERSION`')
            elif MODEL.fullmatch(m.group('model')) is None:
                errors.append(f'`{m.group("model")}` is not a model\'s exact identifier, in '
                              'lowercase and with nothing added to it')
            elif EFFORT.search(m.group('model')) is not None:
                errors.append(f'`{m.group("model")}` carries a reasoning effort, which is no '
                              'part of a model\'s identifier')
        elif key.lower() == 'co-authored-by' and credits_ai(value):
            errors.append(f'`{key}: {value}` credits an AI assistant, which only '
                          '`Assisted-by:` does')


def check_message(text, names):
    """The errors and warnings for one commit message."""
    errors = []
    warnings = []
    if text.strip() == '':
        return ['the message is empty'], warnings
    lines, body, trailers = parse(text)

    check_subject(lines[0], names, errors, warnings)
    if len(lines) > 1 and lines[1] != '':
        errors.append('no blank line follows the subject')
    if len(body) > 3:
        errors.append(f'the body is {len(body)} lines long, past the 3 it may be')
    if any(TRAILER.fullmatch(line) and line.lower().startswith('assisted-by:') for line in body):
        errors.append('the `Assisted-by:` trailer is not in the last paragraph, among the '
                      'trailers')
    if not all(line.isascii() for line in [lines[0]] + body):
        errors.append('the subject or body has a non-ASCII character')
    if any(GENERATED.search(line) for line in lines):
        errors.append('a "Generated with" line credits an AI assistant, which only '
                      '`Assisted-by:` does')
    for line in body:
        if len(line) > WIDTH:
            warnings.append(f'a body line is {len(line)} columns long, past about {WIDTH}')
    check_trailers(trailers, errors)
    return errors, warnings


def escape(text, prop=False):
    """text, escaped for a GitHub Actions workflow command."""
    text = text.replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
    if prop:
        text = text.replace(':', '%3A').replace(',', '%2C')
    return text


def report(name, subject, errors, warnings):
    annotate = os.environ.get('GITHUB_ACTIONS') == 'true'
    print(f'{name}: {subject}')
    for kind, msgs in (('error', errors), ('warning', warnings)):
        for msg in msgs:
            print(f'    {kind}: {msg}')
            if annotate:
                print(f'::{kind} title={escape(name, True)}::{escape(msg)}')


def git(*args):
    return subprocess.run(['git', *args], stdout=subprocess.PIPE, check=True).stdout.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--range', help='the commits to check, as BASE..HEAD')
    group.add_argument('--message-file', help='a file holding the one message to check')
    args = parser.parse_args()

    if args.message_file is not None:
        with open(args.message_file, encoding='utf-8') as f:
            messages = [('message', f.read())]
    else:
        try:
            shas = git('rev-list', '--no-merges', '--reverse', args.range).split()
            messages = [(sha[:12], git('log', '-1', '--no-show-signature', '--format=%B', sha))
                        for sha in shas]
        except (OSError, subprocess.CalledProcessError) as e:
            print(f'cannot list the commits in {args.range}: {e}', file=sys.stderr)
            return 2

    names = tracked_names()
    failed = 0
    for name, text in messages:
        errors, warnings = check_message(text, names)
        if errors or warnings:
            report(name, text.strip('\n').split('\n')[0], errors, warnings)
        if errors:
            failed += 1

    noun = 'commit message' if len(messages) == 1 else 'commit messages'
    if failed != 0:
        print(f'\nFailed: {failed} of {len(messages)} {noun}. See "Commit conventions" in '
              'CONTRIBUTING.md.')
        return 1
    print(f'Passed: {len(messages)} {noun}.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
