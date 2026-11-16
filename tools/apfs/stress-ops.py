#!/usr/bin/env python3
"""Random long workload on one APFS image, to catch the free-list mistakes a real system finds.

The phone's data volume came back from a working session with
    apfs: block N is given back but was free already
    fs: commit failed: 5
on the next boot, after thousands of transactions of a real system (files created, written,
renamed, cut short, given attributes, unlinked).  The short tests in write-tests.py do each of
those once; this does them in an order nobody wrote by hand, on one image, and stops at the
first transaction the writer refuses.

usage: stress-ops.py [ROUNDS] [--image PATH] [--seed N] [--keep]
       default 400 rounds on a fresh image in apfs-work/stress.apfs

Every operation is one apfs-tool run (a fresh open of the image, as on a mounted volume the
library's cache is the difference -- the transaction code under test is the same).
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

TREE = {
    'a': 0, 'b': 1, 'c': BS - 1, 'd': BS, 'e': BS + 1, 'f': 2 * BS,
    'g': 300 * BS, 'sub/h': 1300,
}


def run(*argv, data=None):
    p = subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def make_image(path):
    tree = os.path.join(WORK, 'stress-tree')
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(os.path.join(tree, 'sub'))
    rng = random.Random(7)
    for name, size in sorted(TREE.items()):
        full = os.path.join(tree, name)
        with open(full, 'wb') as f:
            f.write(bytes(rng.randrange(256) for _ in range(size)))
    rc, out = run(sys.executable, os.path.join(HERE, 'mkapfs.py'), '-o', path,
                  '--slack', '2000', 'data=' + tree)
    if rc != 0:
        raise SystemExit('mkapfs.py failed:\n' + out)


def apfsck(image):
    if not os.path.exists(APFSCK):
        return True, ''
    rc, out = run(APFSCK, '-w', image)
    return rc == 0, out


class World:
    def __init__(self, rng):
        self.rng = rng
        self.files = ['/a', '/b', '/c', '/d', '/e', '/f', '/g', '/sub/h']
        self.dirs = ['/sub']
        self.attrs = set()
        self.links = set()
        self.n = 0

    def name(self, prefix='f'):
        self.n += 1
        return '/%s%d' % (prefix, self.n)

    def pick(self):
        """A regular file: symlinks take no writes, truncates or attributes here."""
        regular = [f for f in self.files if f not in self.links]
        return self.rng.choice(regular) if regular else self.files[0]

    def regular(self):
        return [f for f in self.files if f not in self.links]


def split(path):
    """'/sub/h' -> ('/sub', 'h'): the tool's transactions name a directory and one name."""
    d, _, n = path.rpartition('/')
    return (d or '/'), n


def op_create(w, img):
    name = w.name()
    rc, out = run(TOOL, 'txn-create', img, '/', name[1:])
    if rc == 0:
        w.files.append(name)
    return rc, out, 'create %s' % name


def op_append(w, img):
    name = w.pick()
    size = w.rng.choice([1, 17, BS - 1, BS, BS + 5, 3 * BS, 64 * 1024])
    data = bytes(w.rng.randrange(256) for _ in range(size))
    rc, out = run(TOOL, 'txn-append', img, name, '-', '1700000000000000000', data=data)
    return rc, out, 'append %d into %s' % (size, name)


def op_write(w, img):
    name = w.pick()
    at = w.rng.choice([0, 1, BS - 1, BS, BS + 3, 2 * BS, 100 * BS])
    data = bytes(w.rng.randrange(256) for _ in range(w.rng.choice([1, 100, BS, 2 * BS])))
    rc, out = run(TOOL, 'txn-write', img, name, str(at), '-', data=data)
    return rc, out, 'write at %d into %s' % (at, name)


def op_truncate(w, img):
    name = w.pick()
    size = w.rng.choice([0, 1, 100, BS, BS + 1, 2 * BS, 5 * BS, 300 * BS, 400 * BS])
    rc, out = run(TOOL, 'txn-truncate', img, name, str(size))
    return rc, out, 'truncate %s to %d' % (name, size)


def op_rename(w, img):
    name = w.pick()
    new = w.name('r')
    d = w.rng.choice(['/'] + list(w.dirs))
    old_dir, old_name = split(name)
    rc, out = run(TOOL, 'txn-rename', img, old_dir, old_name, d, new[1:])
    if rc == 0:
        moved = (d.rstrip('/') + new)
        w.files[w.files.index(name)] = moved
        if name in w.links:
            w.links.discard(name)
            w.links.add(moved)
        if name in w.attrs:
            w.attrs.discard(name)
            w.attrs.add(moved)
    return rc, out, 'rename %s -> %s%s' % (name, d, new[1:])


def op_link(w, img):
    name = w.pick()
    new = w.name('l')
    rc, out = run(TOOL, 'txn-link', img, '/', new[1:], name)
    if rc == 0:
        w.files.append(new)
    return rc, out, 'link %s -> %s' % (new, name)


def op_symlink(w, img):
    new = w.name('s')
    rc, out = run(TOOL, 'txn-symlink', img, '/', new[1:], 'some/target %d' % w.n)
    if rc == 0:
        w.files.append(new)
        w.links.add(new)
    return rc, out, 'symlink %s' % new


def op_xattr(w, img):
    name = w.pick()
    if name in w.attrs:
        rc, out = run(TOOL, 'txn-rmxattr', img, name, 'user.test')
        if rc == 0:
            w.attrs.discard(name)
        return rc, out, 'rmxattr on %s' % name
    data = bytes(w.rng.randrange(256) for _ in range(w.rng.choice([1, 40, 1000, 3804])))
    rc, out = run(TOOL, 'txn-setxattr', img, name, 'user.test', '-', data=data)
    if rc == 0:
        w.attrs.add(name)
    return rc, out, 'setxattr %d on %s' % (len(data), name)


def op_unlink(w, img):
    if len(w.files) <= 1:
        return 0, '', 'unlink (nothing to take)'
    name = w.pick()
    d, n = split(name)
    rc, out = run(TOOL, 'txn-unlink', img, d, n)
    if rc == 0:
        w.files.remove(name)
    return rc, out, 'unlink %s' % name


def op_mkdir(w, img):
    name = w.name('d')
    rc, out = run(TOOL, 'txn-mkdir', img, '/', name[1:])
    if rc == 0:
        w.dirs.append(name)
    return rc, out, 'mkdir %s' % name


def op_rmdir(w, img):
    if not w.dirs:
        return 0, '', 'rmdir (none)'
    empty = [d for d in w.dirs if not any(f.startswith(d + '/') for f in w.files)]
    if not empty:
        return 0, '', 'rmdir (nothing empty)'
    name = empty[0]
    w.dirs.remove(name)
    rc, out = run(TOOL, 'txn-rmdir', img, '/', name[1:])
    return rc, out, 'rmdir %s' % name


OPS = [
    (op_create, 3), (op_append, 6), (op_write, 4), (op_truncate, 4), (op_rename, 3),
    (op_symlink, 2), (op_xattr, 3), (op_unlink, 4), (op_mkdir, 1), (op_rmdir, 1),
]
LINK_OP = (op_link, 2)


def main(argv):
    rounds = 400
    image = os.path.join(WORK, 'stress.apfs')
    seed = 20261109
    keep = '--keep' in argv
    each = '--check-each' in argv
    args = [a for a in argv if not a.startswith('--')]
    if '--image' in argv:
        image = argv[argv.index('--image') + 1]
    if '--seed' in argv:
        seed = int(argv[argv.index('--seed') + 1])
    if args:
        rounds = int(args[0])

    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(image):
        print('making %s' % image)
        make_image(image)
    rng = random.Random(seed)
    w = World(rng)
    ops = list(OPS)
    if '--links' in argv:
        ops.append(LINK_OP)
    weights = [n for _, n in ops]
    fns = [f for f, _ in ops]

    for r in range(1, rounds + 1):
        fn = op_create if not w.regular() else rng.choices(fns, weights)[0]
        rc, out, what = fn(w, image)
        if rc != 0:
            print('round %d: %s -> rc %d' % (r, what, rc))
            print(out.strip())
            ok, ck = apfsck(image)
            print('apfsck: %s' % ('clean' if ok else ck.strip()[:2000]))
            print('\nlast operations:')
            for line in history[-40:]:
                print('  ' + line)
            with open(os.path.join(WORK, 'stress-history.txt'), 'w') as f:
                f.write('\n'.join(history) + '\n')
            print('\nthe image is %s' % image)
            print('links: %s' % sorted(w.links))
            print('files: %s' % sorted(w.files))
            return 1
        history.append(what)
        if len(history) > 400:
            del history[0]
        if each:
            ok, ck = apfsck(image)
            if not ok:
                print('round %d: %s broke it' % (r, what))
                print(ck.strip()[:2000])
                for line in history[-12:]:
                    print('  ' + line)
                return 1
        if r % 50 == 0:
            ok, ck = apfsck(image)
            print('round %d: apfsck %s' % (r, 'clean' if ok else 'COMPLAINS'))
            if not ok:
                print(ck.strip()[:2000])
                return 1
    ok, ck = apfsck(image)
    print('%d rounds, apfsck %s' % (rounds, 'clean' if ok else ck.strip()[:2000]))
    if not keep:
        pass
    return 0 if ok else 1


history = []

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
