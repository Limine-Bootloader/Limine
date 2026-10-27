import argparse
import fnmatch
import importlib
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import threading
import time
import traceback
from concurrent.futures import ProcessPoolExecutor, as_completed

from harness import api
from harness.env import Env, CHECKSDIR
from harness.ports import PORTS


def load_cases():
    casedir = os.path.join(CHECKSDIR, 'cases')
    for fn in sorted(os.listdir(casedir)):
        if fn.endswith('.py') and not fn.startswith('_'):
            importlib.import_module(f'cases.{fn[:-3]}')
    return api.REGISTRY


STATUS_WORDS = {'PASS': 'passed', 'FAIL': 'failed', 'ERROR': 'errored', 'SKIP': 'skipped'}
STATUS_COLOURS = {'PASS': '32', 'FAIL': '31', 'ERROR': '1;31', 'SKIP': '33'}


def painter(stream):
    """A function colouring text with an SGR code where stream is a terminal,
    unless NO_COLOR is set (no-color.org)."""
    if stream.isatty() and not os.environ.get('NO_COLOR') and os.environ.get('TERM') != 'dumb':
        return lambda text, code: f'\x1b[{code}m{text}\x1b[0m'
    return lambda text, code: text


def build_payloads(env, jobs):
    # The build tree's make knows the toolchain that builds Limine, and the
    # ports it builds. It is GNU make, which some systems call gmake.
    make = os.environ.get('MAKE') or shutil.which('gmake') or 'make'
    argv = [make, '-C', env.builddir, f'-j{jobs}', 'check-payloads']
    res = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if res.returncode != 0:
        sys.stdout.write(res.stdout.decode(errors='replace'))
        raise SystemExit('building the check payloads failed')


class Result:
    def __init__(self, check, port, status, detail='', duration=0.0, workdir=None):
        self.check = check
        self.port = port
        self.status = status
        self.detail = detail
        self.duration = duration
        self.workdir = workdir

    @property
    def id(self):
        return f'{self.check.full_name}[{self.port}]'


def run_one(env, check, port, coverage, keep):
    workdir = os.path.join(env.outdir, check.full_name, port)
    if os.path.exists(workdir):
        shutil.rmtree(workdir)
    ctx = api.Context(env, PORTS[port], workdir, coverage=coverage)
    start = time.monotonic()
    status, detail = 'PASS', ''
    deadline = [start + check.timeout * PORTS[port].timeout_scale]
    done = threading.Event()

    def watchdog():
        while not done.wait(1):
            if time.monotonic() > deadline[0]:
                ctx.soft_failures.append(f'check exceeded its {check.timeout}s budget')
                ctx.stop_all()
                return

    wd = threading.Thread(target=watchdog, daemon=True)
    wd.start()
    try:
        check.func(ctx)
        if ctx.soft_failures:
            status = 'FAIL'
            detail = '\n'.join(ctx.soft_failures)
    except api.Skip as e:
        status, detail = 'SKIP', str(e)
    except (api.CheckFailure, AssertionError, api.CheckTimeout) as e:
        status = 'FAIL'
        detail = '\n'.join(ctx.soft_failures + [f'{e.__class__.__name__}: {e}'])
    except Exception:
        status = 'ERROR'
        detail = '\n'.join(ctx.soft_failures + [traceback.format_exc()])
    finally:
        done.set()
        if status in ('FAIL', 'ERROR'):
            for i, m in enumerate(ctx.machines):
                try:
                    if not m.exited():
                        m.screendump(f'failure-{i}').save_png(os.path.join(m.workdir, f'failure-{i}.png'))
                except Exception:
                    pass
        ctx.stop_all()
    if coverage is not None:
        coverage.collect(workdir)
    duration = time.monotonic() - start
    with open(os.path.join(workdir, 'result.txt'), 'w') as f:
        f.write(f'{status} {check.full_name}[{port}] {duration:.1f}s\n')
        if ctx.notes:
            f.write('\n'.join(ctx.notes) + '\n')
        if detail:
            f.write(detail + '\n')
    if status in ('PASS', 'SKIP') and not keep:
        # Images dominate the size of a run; logs and screenshots stay.
        for root, _, files in os.walk(workdir):
            for fn in files:
                if fn.endswith(('.img', '.iso', '.fd', '.ppm')) and not fn.startswith('keep-'):
                    os.unlink(os.path.join(root, fn))
    return Result(check, port, status, detail, duration, workdir)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Run the Limine checks under QEMU.')
    ap.add_argument('-k', '--filter', action='append', default=[],
                    help='only checks whose name matches this glob (repeatable)')
    ap.add_argument('-p', '--port', action='append', default=[],
                    help=f'only these ports ({", ".join(PORTS)})')
    ap.add_argument('-t', '--tag', action='append', default=[], help='only checks with this tag')
    ap.add_argument('-j', '--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 2) // 2)))
    ap.add_argument('--builddir', help='Limine build directory (default: the source tree)')
    ap.add_argument('--outdir', help='where results go (default: <builddir>/checks-results)')
    ap.add_argument('--ovmfdir', help='directory with the UEFI firmware images')
    ap.add_argument('--no-build', action='store_true', help='do not (re)build the check payloads')
    ap.add_argument('--keep', action='store_true', help='keep disk images of passing checks')
    ap.add_argument('--list', action='store_true', help='list the selected checks and exit')
    ap.add_argument('--coverage', action='store_true',
                    help='record executed Limine code and report line coverage')
    args = ap.parse_args(argv)

    env = Env(args.builddir, args.outdir, args.ovmfdir)
    checks = load_cases()
    selected = []
    # Unless asked for by name, ports the build left out are left out here.
    unbuilt = set()
    for name, port in PORTS.items():
        if not args.port and not os.path.exists(env.bin(port.efi_name if port.is_uefi else 'limine-bios.sys')):
            unbuilt.add(name)
    for c in checks:
        if args.filter and not any(fnmatch.fnmatch(c.full_name, f) or f in c.full_name
                                   for f in args.filter):
            continue
        if args.tag and not set(args.tag) & set(c.tags):
            continue
        for p in c.ports:
            if (args.port and p not in args.port) or p in unbuilt:
                continue
            selected.append((c, p))
    if unbuilt:
        print(f'not built, left out: {", ".join(sorted(unbuilt))}')

    if args.list:
        for c, p in selected:
            print(f'{c.full_name}[{p}]')
        return 0

    if not args.no_build:
        print('Building the programs the checks boot', flush=True)
        build_payloads(env, args.jobs * 2)

    coverage = None
    if args.coverage:
        from harness.coverage import Coverage
        coverage = Coverage(env)

    os.makedirs(env.outdir, exist_ok=True)
    results = []
    start = time.monotonic()
    # A process for each check, so that checks' Python work, such as decoding
    # screens, runs side by side rather than in turns at the interpreter lock.
    os.environ['PYTHONPATH'] = os.pathsep.join(filter(None, (CHECKSDIR, os.environ.get('PYTHONPATH'))))
    # Logging every translated block slows emulation.
    if coverage is not None:
        os.environ.setdefault('LIMINE_CHECK_SLOWDOWN', '3')
        os.environ['LLVM_PROFILE_FILE'] = coverage.host_profile_pattern
    if not selected:
        print('No check runs selected')
        return 0
    paint = painter(sys.stdout)
    total = len(selected)
    width = len(str(total))
    # Details line up with the check's name, after its count, status and time.
    indent = ' ' * len(f'[{total}/{total}] {"PASS":5} {0:6.1f}s  ')
    used = {p for _, p in selected}
    print(f'Running {total} check runs on {", ".join(p for p in PORTS if p in used)}, '
          f'{args.jobs} at a time\n', flush=True)
    with ProcessPoolExecutor(max_workers=args.jobs, mp_context=multiprocessing.get_context('forkserver')) as pool:
        futures = [pool.submit(run_one, env, c, p, coverage, args.keep) for c, p in selected]
        for done, future in enumerate(as_completed(futures), 1):
            r = future.result()
            results.append(r)
            line = f'[{done:{width}}/{total}] {paint(f"{r.status:5}", STATUS_COLOURS[r.status])} {r.duration:6.1f}s  {r.id}'
            if r.status == 'SKIP' and r.detail:
                line += f' ({r.detail.splitlines()[0]})'
            print(line, flush=True)
            if r.status in ('FAIL', 'ERROR'):
                for detail in r.detail.rstrip().split('\n')[:40]:
                    print(f'{indent}{detail}')
                print(f'{indent}artifacts: {r.workdir}')

    counts = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1
    summary = ', '.join(paint(f'{counts[s]} {STATUS_WORDS[s]}', STATUS_COLOURS[s])
                        for s in STATUS_WORDS if s in counts)
    minutes, seconds = divmod(round(time.monotonic() - start), 60)
    elapsed = f'{minutes}m {seconds:02}s' if minutes else f'{seconds}s'
    print(f'\n{len(results)} check runs in {elapsed}: {summary}')
    failed = sorted(r.id for r in results if r.status in ('FAIL', 'ERROR'))
    if failed:
        print('\nFailed:\n' + '\n'.join(f'  {f}' for f in failed))
    with open(os.path.join(env.outdir, 'results.json'), 'w') as f:
        json.dump([{'check': r.check.full_name, 'port': r.port, 'status': r.status,
                    'duration': round(r.duration, 2), 'detail': r.detail} for r in results],
                  f, indent=1)
    if coverage is not None:
        coverage.report(os.path.join(env.outdir, 'coverage'), [r.workdir for r in results])
    return 1 if counts.get('FAIL') or counts.get('ERROR') else 0
