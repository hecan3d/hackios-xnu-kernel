#!/usr/bin/env python3
"""A long workload as one apfs-tool process: the volume is opened once, every step is a
transaction on it, as on a mounted volume in the kernel.

stress-ops.py opens the image for each step, which is right for what it tests but hides
everything that depends on the state a mount keeps in memory (the roots a transaction leaves
behind, the object map's cached blocks).  The phone's data volume, mounted for a whole session,
is where those show: after a few thousand changes the kernel began to refuse inode inserts
with EIO.  This runs the same kind of load the way the kernel does.

usage: stress-batch.py [ROUNDS] [--image PATH] [--script PATH] [--seed N]
       default 3000 rounds on a fresh image in apfs-work/batch.apfs
"""
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(ROOT, 'apfs-work')
TOOL = os.path.join(WORK, 'apfs-tool')
APFSCK = os.path.join(ROOT, 'xnu-deps/build/apfsprogs/apfsck/apfsck')
BS = 4096

TREE = {'a': 0, 'b': 1, 'c': BS - 1, 'd': BS, 'e': BS + 1, 'f': 2 * BS, 'g': 300 * BS,
        'sub/h': 1300}


def run(*argv):
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def make_image(path):
    tree = os.path.join(WORK, 'batch-tree')
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(os.path.join(tree, 'sub'))
    rng = random.Random(7)
    for name, size in sorted(TREE.items()):
        with open(os.path.join(tree, name), 'wb') as f:
            f.write(bytes(rng.randrange(256) for _ in range(size)))
    rc, out = run(sys.executable, os.path.join(HERE, 'mkapfs.py'), '-o', path,
                  '--slack', '20000', 'data=' + tree)
    if rc != 0:
        raise SystemExit('mkapfs.py failed:\n' + out)


class World:
    def __init__(self, rng):
        self.rng = rng
        self.files = ['/a', '/b', '/c', '/d', '/e', '/f', '/g', '/sub/h']
        self.dirs = ['/sub']
        self.links = set()
        self.attrs = set()
        self.n = 0
        self.lines = []

    def name(self, prefix):
        self.n += 1
        return '%s%d' % (prefix, self.n)

    def regular(self):
        return [f for f in self.files if f not in self.links]

    def pick(self):
        r = self.regular()
        return self.rng.choice(r) if r else None

    def split(self, path):
        d, _, n = path.rpartition('/')
        return d or '/', n


def generate(rounds, seed):
    """Two pools: a stable one (never renamed or unlinked) for the operations that name an
    inode -- attributes, hard links, time -- and a churning one for the rest.  The bookkeeping
    that a script with one pool needs is a test of the generator, not of the volume."""
    rng = random.Random(seed)
    w = World(rng)
    w.lines.append('mkdir / keep')
    for i in range(12):
        w.lines.append('create /keep s%d' % i)
    w.dirs.append('/keep')
    stable = ['/keep/s%d' % i for i in range(12)]
    w.files = list(stable) + ['/a', '/b', '/c', '/d', '/e', '/f', '/g', '/sub/h']

    def churnable():
        c = [f for f in w.files if f not in stable and f not in w.links]
        return rng.choice(c) if c else None

    def create():
        n = w.name('f')
        d = rng.choice(['/'] + w.dirs)
        w.lines.append('create %s %s' % (d, n))
        w.files.append(d.rstrip('/') + '/' + n)

    def mkdir():
        n = w.name('d')
        w.lines.append('mkdir / %s' % n)
        w.dirs.append('/' + n)

    def rmdir():
        empty = [d for d in w.dirs if d != '/keep' and
                 not any(f.startswith(d + '/') for f in w.files)]
        if not empty:
            return
        d = empty[0]
        w.dirs.remove(d)
        w.lines.append('rmdir %s %s' % w.split(d))

    def unlink():
        name = churnable()
        if name is None:
            return
        w.files.remove(name)
        w.lines.append('unlink %s %s' % w.split(name))

    def rename():
        name = churnable()
        if name is None:
            return
        new = w.name('r')
        d = rng.choice(['/'] + w.dirs)
        w.lines.append('rename %s %s %s %s' % (w.split(name) + (d, new)))
        moved = d.rstrip('/') + '/' + new
        w.files[w.files.index(name)] = moved

    def append():
        name = churnable() or rng.choice(stable)
        w.lines.append('append %s %d' % (name, rng.choice([1, 17, 100, BS - 1, BS, BS + 5,
                                                           3 * BS, 64 * 1024])))

    def write():
        name = churnable() or rng.choice(stable)
        w.lines.append('write %s %d %d' % (name,
                                           rng.choice([0, 1, BS - 1, BS, BS + 3, 2 * BS, 100 * BS]),
                                           rng.choice([1, 100, BS, 2 * BS])))

    def truncate():
        name = churnable() or rng.choice(stable)
        w.lines.append('truncate %s %d' % (name, rng.choice([0, 1, 100, BS, BS + 1, 2 * BS,
                                                             5 * BS, 300 * BS, 400 * BS])))

    def link():
        target = rng.choice(stable)
        new = w.name('l')
        w.lines.append('link / %s %s' % (new, target))
        w.files.append('/' + new)

    def symlink():
        new = w.name('s')
        w.lines.append('symlink / %s target.%d' % (new, w.n))
        w.files.append('/' + new)
        w.links.add('/' + new)

    def xattr():
        name = rng.choice(stable)
        if name in w.attrs:
            w.attrs.discard(name)
            w.lines.append('rmxattr %s user.test' % name)
        else:
            w.attrs.add(name)
            w.lines.append('setxattr %s user.test %d' % (name,
                                                         rng.choice([1, 40, 1000, 3804])))

    def mtime():
        name = rng.choice(stable)
        w.lines.append('mtime %s %d' % (name, 1700000000 + rng.randrange(10**6)))

    ops = [(create, 5), (append, 5), (write, 4), (truncate, 4), (rename, 3), (symlink, 2),
           (xattr, 3), (unlink, 3), (mkdir, 1), (rmdir, 1), (link, 1), (mtime, 1)]
    fns = [f for f, _ in ops]
    weights = [n for _, n in ops]

    for _ in range(rounds):
        rng.choices(fns, weights)[0]()
    return w.lines


def main(argv):
    rounds = 3000
    image = os.path.join(WORK, 'batch.apfs')
    script = os.path.join(WORK, 'batch-steps.txt')
    seed = 20261109
    args = [a for a in argv if not a.startswith('--')]
    if '--image' in argv:
        image = argv[argv.index('--image') + 1]
    if '--script' in argv:
        script = argv[argv.index('--script') + 1]
    if '--seed' in argv:
        seed = int(argv[argv.index('--seed') + 1])
    if args:
        rounds = int(args[0])

    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(image):
        print('making %s' % image)
        make_image(image)
    lines = generate(rounds, seed)
    with open(script, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d steps in one session' % len(lines))
    rc, out = run(TOOL, 'batch', image, script)
    if rc != 0:
        print(out.strip()[-3000:])
        print('\nlast steps:')
        for line in lines[-30:]:
            print('  ' + line)
        return 1
    print(out.strip()[-200:])
    if os.path.exists(APFSCK):
        rc, out = run(APFSCK, '-w', image)
        print('apfsck: %s' % ('clean' if rc == 0 else out.strip()[:2000]))
        return 0 if rc == 0 else 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
