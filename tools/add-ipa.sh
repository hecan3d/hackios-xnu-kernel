#!/bin/bash
# add-ipa.sh — put the apps of .ipa files into the HakiOS system image as preinstalled apps.
#
# usage: add-ipa.sh [options] IPA [IPA ...]
#        add-ipa.sh [options] --dir DIR
#
# The image is split into flashboot and system at OFFSET; the system tree is modified in place
# and rebuilt with tools/apfs/mkapfs.py -z. The first run extracts the tree; later runs reuse it.
# The original image on the Desktop is never touched. A copy of the image being replaced is kept
# as .bak unless --no-backup is given.
#
# An .ipa is a zip: Payload/<App>.app. The app goes to /Applications on the system volume, where
# LaunchServices finds it by itself at boot (userland/build.sh does the same with the prefix's apps).
# The app's data container is made by containermanagerd on its first launch. The app's own signature
# is kept as the .ipa has it; the port's AMFI stub does not enforce it. An encrypted App Store .ipa
# (cryptid != 0) will not run; use a decrypted one.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-"$REPO/work-sileo"}
IMAGE=${IMAGE:-"/mnt/c/Users/kola/Documents/hackios/images/system/ram-resident.apfs"}
OFFSET=${OFFSET:-112627712}
FORCE=0
BACKUP=1
DRY=0
DIR=""
IPAS=()

usage() {
	cat <<EOF
usage: $0 [options] IPA [IPA ...]
       $0 [options] --dir DIR

Put the apps of .ipa files into the system image as preinstalled applications.

options:
  --dir DIR       take every .ipa in DIR
  --image PATH    the ram-resident.apfs to update (default: $IMAGE)
  --work DIR      the split/extracted work directory (default: $WORK)
  --force         replace an app already in the image
  --no-backup     do not keep a copy of the old image as .bak
  --dry-run       extract and report, change nothing
  -h, --help      this text

The image is split at $OFFSET bytes (flashboot + system). The system tree is
modified in place and rebuilt with tools/apfs/mkapfs.py -z. The first run
extracts the tree; later runs reuse it. The original image on the Desktop is
never touched. CHECK=1 also runs tools/apfs/check-image.sh on the new system.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--dir) DIR=$2; shift 2 ;;
		--image) IMAGE=$2; shift 2 ;;
		--work) WORK=$2; shift 2 ;;
		--force) FORCE=1; shift ;;
		--no-backup) BACKUP=0; shift ;;
		--dry-run) DRY=1; shift ;;
		-h|--help) usage; exit 0 ;;
		--) shift; break ;;
		-*) echo "add-ipa.sh: unknown option: $1" >&2; exit 1 ;;
		*) break ;;
	esac
done
IPAS=("$@")

if [ -n "$DIR" ]; then
	mapfile -t DIR_IPAS < <(find "$DIR" -maxdepth 1 -type f -iname '*.ipa' | sort)
	IPAS+=("${DIR_IPAS[@]}")
fi
[ ${#IPAS[@]} -gt 0 ] || { echo "add-ipa.sh: no .ipa files given" >&2; usage >&2; exit 1; }

conv() {
	case "$1" in
		[A-Za-z]:[\\/]*) wslpath -u "$1" ;;
		*) printf '%s\n' "$1" ;;
	esac
}

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f flashboot-split.apfs ] || [ ! -d system-tree ]; then
	[ -f "$IMAGE" ] || { echo "add-ipa.sh: image not found: $IMAGE" >&2; exit 1; }
	echo "==> splitting $IMAGE at $OFFSET"
	head -c "$OFFSET" "$IMAGE" > flashboot-split.apfs || exit 1
	tail -c +$((OFFSET + 1)) "$IMAGE" > system.apfs || exit 1
	echo "==> extracting system.apfs"
	rm -rf system-tree
	if ! 7z x -y -osystem-tree system.apfs > extract.log 2>&1; then
		echo "add-ipa.sh: 7z failed; see $WORK/extract.log" >&2
		exit 1
	fi
	python3 - <<'PY'
import os
root = 'system-tree'
pref = os.path.abspath(root) + '/'
fixed = 0
for dp, dn, fn in os.walk(root):
    for n in dn + fn:
        p = os.path.join(dp, n)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith(pref):
                os.remove(p)
                os.symlink('/' + t[len(pref):], p)
                fixed += 1
print('fixed %d host-absolute symlinks' % fixed)
PY
fi

added=0
skipped=0
for ipa in "${IPAS[@]}"; do
	ipa=$(conv "$ipa")
	if [ ! -f "$ipa" ]; then
		echo "skip: not a file: $ipa" >&2
		skipped=$((skipped + 1))
		continue
	fi
	tmp=$(mktemp -d)
	echo "==> extracting $(basename "$ipa")"
	if ! unzip -q "$ipa" -d "$tmp"; then
		echo "skip: unzip failed: $ipa" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	app=$(find "$tmp/Payload" -maxdepth 1 -type d -name '*.app' 2>/dev/null | head -n1)
	[ -n "$app" ] || app=$(find "$tmp" -maxdepth 1 -type d -name '*.app' 2>/dev/null | head -n1)
	if [ -z "$app" ]; then
		echo "skip: no .app in $ipa" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	name=$(basename "$app")
	dest="system-tree/Applications/$name"
	if [ -e "$dest" ] && [ "$FORCE" -ne 1 ]; then
		echo "skip: $name is already in the image (use --force to replace)" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	bundle=$(python3 - "$app/Info.plist" <<'PY' 2>/dev/null || true
import plistlib, sys
try:
    d = plistlib.load(open(sys.argv[1], 'rb'))
    print(d.get('CFBundleIdentifier', ''))
except Exception:
    pass
PY
)
	exe=$(python3 - "$app/Info.plist" <<'PY' 2>/dev/null || true
import plistlib, sys
try:
    d = plistlib.load(open(sys.argv[1], 'rb'))
    print(d.get('CFBundleExecutable', ''))
except Exception:
    pass
PY
)
	if [ -n "$exe" ] && [ -f "$app/$exe" ]; then
		cryptid=$(python3 - "$app/$exe" <<'PY' 2>/dev/null || echo 0
import struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    magic = f.read(4)
    if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe'):
        f.seek(0)
        data = f.read(65536)
        ncmds = struct.unpack_from('<I', data, 16)[0]
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', data, off)
            if cmd in (0x21, 0x2c):
                print(struct.unpack_from('<I', data, off + 16)[0])
                break
            off += cmdsize
        else:
            print(0)
    else:
        print(0)
PY
)
	else
		cryptid=0
	fi
	echo "==> adding $name${bundle:+ ($bundle)}"
	if [ "${cryptid:-0}" -gt 0 ] 2>/dev/null; then
		echo "    warning: encrypted binary (cryptid=$cryptid); use a decrypted .ipa" >&2
	fi
	if [ "$DRY" -eq 1 ]; then
		echo "    dry run: would copy to $dest"
		rm -rf "$tmp"
		added=$((added + 1))
		continue
	fi
	rm -rf "$dest"
	cp -a "$app" "$dest" || { echo "skip: copy failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
	find "$dest" -type d -exec chmod 755 {} +
	find "$dest" -type f -exec chmod 644 {} +
	[ -n "$exe" ] && [ -f "$dest/$exe" ] && chmod 755 "$dest/$exe"
	for d in Frameworks PlugIns Watch XPCServices; do
		[ -d "$dest/$d" ] && find "$dest/$d" -type f -exec chmod 755 {} + 2>/dev/null
	done
	echo "    $(du -sh "$dest" | cut -f1)"
	rm -rf "$tmp"
	added=$((added + 1))
done

if [ "$added" -eq 0 ]; then
	echo "add-ipa.sh: nothing added ($skipped skipped)" >&2
	exit 1
fi

if [ "$DRY" -eq 1 ]; then
	echo "==> dry run: would rebuild the system image and write $IMAGE"
	exit 0
fi

echo "==> rebuilding the system image"
if ! python3 "$REPO/tools/apfs/mkapfs.py" -z -o system-new.apfs System=system-tree > mkfs.log 2>&1; then
	echo "add-ipa.sh: mkapfs.py failed; see $WORK/mkfs.log" >&2
	tail -20 mkfs.log >&2
	exit 1
fi
if [ "${CHECK:-0}" = 1 ] && [ -x "$REPO/tools/apfs/check-image.sh" ]; then
	"$REPO/tools/apfs/check-image.sh" system-new.apfs System=system-tree || { echo "add-ipa.sh: check failed" >&2; exit 1; }
fi
cat flashboot-split.apfs system-new.apfs > ram-resident-new.apfs || exit 1

if [ "$BACKUP" -eq 1 ] && [ -f "$IMAGE" ]; then
	free=$(df -B1 --output=avail "$(dirname "$IMAGE")" 2>/dev/null | tail -1)
	size=$(stat -c %s "$IMAGE")
	if [ -n "$free" ] && [ "$free" -lt "$size" ]; then
		echo "add-ipa.sh: not enough free space for a backup; skipping it" >&2
	else
		echo "==> backup: $IMAGE.bak"
		cp -f "$IMAGE" "$IMAGE.bak" || echo "add-ipa.sh: backup failed" >&2
	fi
fi
echo "==> writing $IMAGE"
cp -f ram-resident-new.apfs "$IMAGE" || exit 1
sync
size=$(stat -c %s "$IMAGE")
sha=$(sha256sum "$IMAGE" | awk '{print $1}')
echo "==> done"
echo "image:  $IMAGE"
echo "size:   $size bytes"
echo "sha256: $sha"
echo "added:  $added, skipped: $skipped"
if [ "$size" -gt 4000000000 ]; then
	echo "warning: the image is over 4 GB; the loader may not fit it in RAM" >&2
fi

sums="/mnt/c/Users/kola/Documents/hackios/docs/SHA256SUMS.txt"
if [ -f "$sums" ]; then
	python3 - "$sums" "$sha" <<'PY'
import sys
p, sha = sys.argv[1], sys.argv[2]
lines = open(p).read().splitlines()
for i, l in enumerate(lines):
    if l.endswith('images/system/ram-resident.apfs'):
        lines[i] = sha + '  images/system/ram-resident.apfs'
open(p, 'w').write('\n'.join(lines) + '\n')
PY
	echo "updated: $sums"
fi
