#!/usr/bin/env python
"""Where the threads of a game spend a trace of capture.ps1, phase by phase.

  analyze.py <trace.etl> <process.exe> [--pdb <folder>] [--frame-ms on=12.4,off=8]

Every figure is a share of the phase's wall time, so 100% is one thread busy
all the time. With --frame-ms they are also given in ms a frame.

Samples say where a thread runs and context switches where it waits. Other
threads and the GPU can limit a frame while a thread here is far from busy.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

XPERF = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                     'Windows Kits', '10', 'Windows Performance Toolkit', 'xperf.exe')
OWN = 'd3d11.dll'
RUN = re.compile(r'^(\w+)::run$')
# What runs commands and picks the next one, and not what a command does
INTERPRETER = {'_RunCommandList', 'run_command_list_commands<0>', 'run_command_list_commands<1>',
               'RunCommandListComplete', 'CommandList::ResolveCommandList'}

# (name, frames of the DLL): the first group from the leaf upwards with a frame
# on the stack takes the sample. Leaf-most work wins over what called it.
GROUPS = [
    ('waiting for a lock', ('_EnterCriticalSectionPretty',)),
    ('spike log measurements', ('Profiling::Spike::',)),
    ('D3D12 barriers', ('Dx12ShimTransition',)),
    ('expression evaluation', ('CommandListExpression::evaluate',)),
    ('Vulkan mod draw recording', ('record_mod_draws',)),
    ('Vulkan game buffer publishing', ('VkScopeConstantBuffer', 'share_from_gpu', 'replace_buffers')),
    ('Vulkan shader-resource lookup', ('VkScopeShaderResource', 'VkScopePrefetchShaderResources')),
    ('resource hash lookup', ('GetResourceHandleInfo',)),
    ('D3D11-on-D3D12 buffer copies', ('copy_buffer',)),
    ('custom shader state', ('Dx12ShimPrepareDispatch', 'Dx12ShimPrepareDraw', 'ShimContext::Dispatch')),
    ('other D3D11 calls on the shim', ('ShimContext::', 'ShimChild<')),
]


def frame_parts(frame):
    image, _, function = frame.partition('!')
    return image.strip().lower(), function.strip()


class Thread:
    def __init__(self, tid):
        self.tid = tid
        self.samples = 0
        self.images = collections.Counter()
        self.self_own = collections.Counter()
        self.inclusive = collections.Counter()
        self.commands = collections.Counter()
        self.groups = collections.Counter()
        self.outside_lists = collections.Counter()
        self.called_out = collections.Counter()
        self.waits = collections.Counter()
        self.wait_us = 0
        self.under_list = 0
        self.under_draw = 0


class Phase:
    def __init__(self, name):
        self.name = name
        self.start = self.end = None
        self.threads = {}

    def thread(self, tid):
        if tid not in self.threads:
            self.threads[tid] = Thread(tid)
        return self.threads[tid]


def group_of(frames):
    for image, function in frames:
        if image != OWN:
            continue
        for name, members in GROUPS:
            for member in members:
                if function.startswith(member):
                    return name
    return None


def account_sample(thread, frames, count):
    thread.samples += count
    leaf_image, leaf_function = frames[0] if frames else ('(no stack)', '')
    thread.images[leaf_image] += count
    seen = set()
    last_own = None
    command = None
    under_list = False
    for image, function in frames:
        if image != OWN:
            continue
        if last_own is None:
            last_own = function
        if function not in seen:
            seen.add(function)
            thread.inclusive[function] += count
        if command is None and RUN.match(function):
            command = RUN.match(function)[1]
        if function in INTERPRETER:
            under_list = True
    if leaf_image == OWN:
        thread.self_own[leaf_function] += count
    elif last_own is not None:
        thread.called_out[last_own + ' -> ' + leaf_image] += count
    if not under_list:
        if last_own is not None:
            thread.outside_lists[last_own] += count
        return
    thread.under_list += count
    thread.commands[command or '(between commands)'] += count
    if leaf_image == OWN and leaf_function in INTERPRETER:
        thread.groups['interpreter loop'] += count
        return
    group = group_of(frames)
    if group:
        thread.groups[group] += count
    elif leaf_image == OWN:
        thread.groups['other DLL code in commands'] += count
    else:
        thread.groups['other calls out in commands'] += count


def parse(trace, process, pdb, symcache):
    env = dict(os.environ)
    if pdb:
        env['_NT_SYMBOL_PATH'] = pdb
    env['_NT_SYMCACHE_PATH'] = symcache
    # -tle and -tti: a loaded system loses some events, which only thins the samples
    xperf = subprocess.Popen([XPERF, '-tle', '-tti', '-i', trace, '-symbols', '-a', 'dumper', '-stacktimeshifting'],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    needle = (' ' + process + ' (').encode('ascii', 'replace')
    phases = []
    current = None
    state = None
    stack = []
    lost = None
    sample_ms = 1.0

    def flush():
        nonlocal state, stack
        if state is None:
            return
        kind, phase, tid, extra, count = state
        frames = [frame_parts(f) for f in stack]
        thread = phase.thread(tid)
        if kind == 'sample':
            account_sample(thread, frames, count)
        elif frames:
            for image, function in frames:
                if image == OWN:
                    extra[tid] = function
                    break
            else:
                extra[tid] = frames[0][0]
        state = None
        stack = []

    out_at = {}
    switched_out = {}
    for raw in xperf.stdout:
        line = raw.lstrip()
        if line.startswith(b'Stack,'):
            if state is not None:
                fields = raw.decode('utf-8', 'replace').split(',', 5)
                stack.append(fields[5].strip())
            continue
        if state is not None:
            flush()
        if line.startswith(b'SampledProfile,'):
            if current is None or needle not in raw:
                continue
            fields = raw.decode('utf-8', 'replace').split(',', 4)
            if fields[1].strip() == 'TimeStamp':
                continue
            state = ('sample', current, int(fields[3]), None, int(raw.rsplit(b',', 2)[1]))
            stack = []
        elif line.startswith(b'CSwitch,'):
            if current is None or needle not in raw:
                continue
            fields = raw.decode('utf-8', 'replace').split(',')
            if fields[1].strip() == 'TimeStamp':
                continue
            at, new_process, new_tid = int(fields[1]), fields[2], int(fields[3])
            old_process, old_tid = fields[8], int(fields[9])
            if process + ' (' in new_process and new_tid in out_at:
                wait_us = at - out_at.pop(new_tid)
                thread = current.thread(new_tid)
                thread.wait_us += wait_us
                thread.waits[switched_out.get(new_tid, '(no stack)')] += wait_us
            if process + ' (' in old_process:
                out_at[old_tid] = at
                state = ('switch', current, old_tid, switched_out, 0)
                stack = []
        elif line.startswith(b'ProfileFreq-SetInterval,'):
            fields = raw.decode('utf-8', 'replace').split(',')
            if fields[1].strip() != 'TimeStamp':
                # In units of 100 ns; the last one set is the one that applies
                sample_ms = int(fields[5]) / 10000
        elif line.startswith(b'Mark,'):
            fields = raw.decode('utf-8', 'replace').split(',', 2)
            found = re.search(r'xxmi-phase-(start|end) (\S+)', fields[2])
            if found:
                at = int(fields[1])
                if found[1] == 'start':
                    current = Phase(found[2])
                    current.start = at
                    phases.append(current)
                elif current is not None:
                    current.end = at
                    current = None
        elif line.startswith(b'OS Version:'):
            found = re.search(rb'Events Lost: (\d+)', raw)
            if found:
                lost = int(found[1])
    xperf.wait()
    return phases, lost, sample_ms


def table(title, counter, unit, limit=25, least=0.1):
    total = sum(counter.values())
    if not total:
        return
    print('  ' + title)
    for name, value in counter.most_common(limit):
        share = value * unit
        if share < least:
            break
        print('  %7.2f%%  %s' % (share, name[:110]))


def report(phase, frame_ms, sample_ms):
    seconds = (phase.end - phase.start) / 1e6
    unit = sample_ms / (seconds * 1000) * 100
    per_frame = frame_ms.get(phase.name)
    print('=== phase %s: %.1f s, a sample every %.3f ms%s' % (phase.name, seconds, sample_ms,
                                     ', %.2f ms a frame' % per_frame if per_frame else ''))
    threads = sorted(phase.threads.values(), key=lambda t: t.samples, reverse=True)
    print('  Threads by time running, with the image the samples were in:')
    for thread in threads[:10]:
        images = ', '.join('%s %.1f%%' % (i, n * unit) for i, n in thread.images.most_common(4))
        print('  %7.2f%%  thread %-6d off CPU %5.1f%%  in command lists %5.1f%%  %s' % (
            thread.samples * unit, thread.tid, thread.wait_us / (seconds * 1e6) * 100,
            thread.under_list * unit, images))
    print('  All threads: %.1f%% of one core' % sum(t.samples * unit for t in threads))
    workers = [t for t in threads if t.under_list]
    workers.sort(key=lambda t: t.under_list, reverse=True)
    for thread in workers[:2]:
        print('--- thread %d, the one with most command list work' % thread.tid)
        if per_frame:
            print('  command lists: %.2f%% of wall time = %.2f ms a frame' % (
                thread.under_list * unit, thread.under_list * unit * per_frame / 100))
        table('What the time in command lists went to:', thread.groups, unit)
        table('Innermost command on the stack:', thread.commands, unit)
        table('DLL work outside command lists, by its innermost DLL function:', thread.outside_lists, unit)
        table('Self time in the DLL:', thread.self_own, unit)
        table('Calls out of the DLL, by the last DLL function:', thread.called_out, unit)
        table('Inclusive by function of the DLL:', thread.inclusive, unit, limit=30)
        table('Off CPU, by what the thread left from (context switches):',
              collections.Counter({k: v / (seconds * 1e6) * 100 / unit for k, v in thread.waits.items()}), unit, limit=12, least=0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('trace')
    parser.add_argument('process')
    parser.add_argument('--pdb', help='folder of the d3d11.pdb of the DLL that was running')
    parser.add_argument('--frame-ms', default='', help='frame time of each phase from the spike log: on=12.4,off=8')
    args = parser.parse_args()
    frame_ms = {k: float(v) for k, v in (p.split('=') for p in args.frame_ms.split(',') if p)}
    symcache = os.path.join(tempfile.gettempdir(), 'xxmi-etw-symcache')
    os.makedirs(symcache, exist_ok=True)
    phases, lost, sample_ms = parse(args.trace, args.process, args.pdb, symcache)
    if lost:
        print('Events lost by the trace: %d' % lost)
    if not phases:
        print('no phases found: is this a trace of capture.ps1?', file=sys.stderr)
        return 1
    for phase in phases:
        if phase.end is None:
            print('phase %s has no end, skipped' % phase.name)
            continue
        if not phase.threads:
            print('phase %s: nothing of %s in it' % (phase.name, args.process))
            continue
        report(phase, frame_ms, sample_ms)
    return 0


if __name__ == '__main__':
    sys.exit(main())
