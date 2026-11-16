#!/bin/bash
# add-deb.sh — put .deb packages into the HakiOS system image as preinstalled packages.
#
# usage: add-deb.sh [options] DEB [DEB ...]
#        add-deb.sh [options] --dir DIR
#
# The image is split into flashboot and system at OFFSET; the system tree is modified in place
# and rebuilt with tools/apfs/mkapfs.py -z. The first run extracts the tree; later runs reuse it.
# The original image on the Desktop is never touched. A copy of the image being replaced is kept
# as .bak unless --no-backup is given.
#
# A .deb is an ar archive with control.tar.* and data.tar.*. The data goes into the system
# tree (usually under /var/jb for rootless packages) and the control's metadata is appended
# to /var/jb/Library/dpkg/status so dpkg knows it is installed. The package's own postinst
# is NOT run here; run it manually in the phone if a package needs configuration.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-"$REPO/work-sileo"}
IMAGE=${IMAGE:-"/mnt/c/Users/kola/Documents/hackios/images/system/ram-resident.apfs"}
OFFSET=${OFFSET:-112627712}
FORCE=0
BACKUP=1
DRY=0
ENSURE_ONLY=0
REBUILD_ONLY=0
DIR=""
DEBS=()

usage() {
	cat <<EOF
usage: $0 [options] DEB [DEB ...]
       $0 [options] --dir DIR

Put .deb packages into the system image as preinstalled packages.

options:
  --dir DIR       take every .deb in DIR
  --image PATH    the ram-resident.apfs to update (default: $IMAGE)
  --work DIR      the split/extracted work directory (default: $WORK)
  --force         replace a package already in the image
  --no-backup     do not keep a copy of the old image as .bak
  --dry-run       extract and report, change nothing
  --ensure-only   just make sure the work tree is ready and exit
  --rebuild-only  skip adding packages and just rebuild the image
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
		--ensure-only) ENSURE_ONLY=1; shift ;;
		--rebuild-only) REBUILD_ONLY=1; shift ;;
		-h|--help) usage; exit 0 ;;
		--) shift; break ;;
		-*) echo "add-deb.sh: unknown option: $1" >&2; exit 1 ;;
		*) break ;;
	esac
done
DEBS=("$@")

if [ -n "$DIR" ]; then
	mapfile -t DIR_DEBS < <(find "$DIR" -maxdepth 1 -type f -iname '*.deb' | sort)
	DEBS+=("${DIR_DEBS[@]}")
fi

if [ ${#DEBS[@]} -eq 0 ] && [ "$ENSURE_ONLY" -eq 0 ] && [ "$REBUILD_ONLY" -eq 0 ]; then
	echo "add-deb.sh: no .deb files given" >&2
	usage >&2
	exit 1
fi

conv() {
	case "$1" in
		[A-Za-z]:[\\/]*) wslpath -u "$1" ;;
		*) printf '%s\n' "$1" ;;
	esac
}

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f flashboot-split.apfs ] || [ ! -d system-tree ]; then
	[ -f "$IMAGE" ] || { echo "add-deb.sh: image not found: $IMAGE" >&2; exit 1; }
	echo "==> splitting $IMAGE at $OFFSET"
	head -c "$OFFSET" "$IMAGE" > flashboot-split.apfs || exit 1
	tail -c +$((OFFSET + 1)) "$IMAGE" > system.apfs || exit 1
	echo "==> extracting system.apfs"
	rm -rf system-tree
	if ! 7z x -y -osystem-tree system.apfs > extract.log 2>&1; then
		echo "add-deb.sh: 7z failed; see $WORK/extract.log" >&2
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

if [ "$ENSURE_ONLY" -eq 1 ]; then
	echo "==> work tree ready at $WORK/system-tree"
	exit 0
fi

added=0
skipped=0
for deb in "${DEBS[@]}"; do
	deb=$(conv "$deb")
	if [ ! -f "$deb" ]; then
		echo "skip: not a file: $deb" >&2
		skipped=$((skipped + 1))
		continue
	fi
	tmp=$(mktemp -d)
	echo "==> extracting $(basename "$deb")"
	if ! ar x "$deb" --output="$tmp" 2>/dev/null; then
		(cd "$tmp" && ar x "$deb") || { echo "skip: ar failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
	fi
	control_arc=$(find "$tmp" -maxdepth 1 -name 'control.tar.*' | head -n1)
	data_arc=$(find "$tmp" -maxdepth 1 -name 'data.tar.*' | head -n1)
	if [ -z "$control_arc" ] || [ -z "$data_arc" ]; then
		echo "skip: no control.tar.* or data.tar.* in $deb" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	mkdir -p "$tmp/data" "$tmp/control"
	case "$data_arc" in
		*.xz) tar -xJf "$data_arc" -C "$tmp/data" ;;
		*.gz) tar -xzf "$data_arc" -C "$tmp/data" ;;
		*.bz2) tar -xjf "$data_arc" -C "$tmp/data" ;;
		*.lzma) lzma -dc "$data_arc" | tar -xf - -C "$tmp/data" ;;
		*.zst) zstd -dc "$data_arc" | tar -xf - -C "$tmp/data" ;;
		*) tar -xf "$data_arc" -C "$tmp/data" ;;
	esac || { echo "skip: data tar failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
	case "$control_arc" in
		*.xz) tar -xJf "$control_arc" -C "$tmp/control" ;;
		*.gz) tar -xzf "$control_arc" -C "$tmp/control" ;;
		*.bz2) tar -xjf "$control_arc" -C "$tmp/control" ;;
		*.lzma) lzma -dc "$control_arc" | tar -xf - -C "$tmp/control" ;;
		*.zst) zstd -dc "$control_arc" | tar -xf - -C "$tmp/control" ;;
		*) tar -xf "$control_arc" -C "$tmp/control" ;;
	esac || { echo "skip: control tar failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
	control_file="$tmp/control/control"
	[ -f "$control_file" ] || control_file=$(find "$tmp/control" -name control -type f | head -n1)
	if [ -z "$control_file" ] || [ ! -f "$control_file" ]; then
		echo "skip: no control file" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	pkg_name=$(awk -F': ' '/^Package:/{print $2; exit}' "$control_file")
	if [ -z "$pkg_name" ]; then
		echo "skip: no Package field" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	dest="system-tree"
	status_file=""
	for candidate in "$dest/jb/Library/dpkg/status" "$dest/var/jb/Library/dpkg/status"; do
		if [ -f "$candidate" ]; then
			status_file="$candidate"
			break
		fi
	done
	if [ -f "$status_file" ] && grep -q "^Package: ${pkg_name}$" "$status_file" 2>/dev/null && [ "$FORCE" -ne 1 ]; then
		echo "skip: $pkg_name is already in the image (use --force to replace)" >&2
		rm -rf "$tmp"
		skipped=$((skipped + 1))
		continue
	fi
	echo "==> adding $pkg_name"
	if [ "$DRY" -eq 1 ]; then
		echo "    dry run: would copy to $dest"
		rm -rf "$tmp"
		added=$((added + 1))
		continue
	fi
	if [ -d "$tmp/data/var/jb" ]; then
		mkdir -p "$dest/jb"
		cp -a "$tmp/data/var/jb/." "$dest/jb/" || { echo "skip: copy failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
		find "$tmp/data" -mindepth 1 -maxdepth 1 ! -name var -exec cp -a {} "$dest/" \; 2>/dev/null
	else
		cp -a "$tmp/data/." "$dest/" || { echo "skip: copy failed" >&2; rm -rf "$tmp"; skipped=$((skipped + 1)); continue; }
	fi
	if [ -f "$status_file" ]; then
		python3 - "$status_file" "$control_file" <<'PY'
import sys, re
status_path, control_path = sys.argv[1], sys.argv[2]
fields = {}
key = None
for line in open(control_path, encoding='utf-8', errors='replace'):
    line = line.rstrip('\n')
    if line.startswith(' ') or line.startswith('\t'):
        if key:
            fields[key] += '\n' + line
        continue
    if ':' in line:
        key, value = line.split(':', 1)
        fields[key] = value.strip()
    else:
        key = None
existing = open(status_path, encoding='utf-8', errors='replace').read()
pkg = fields.get('Package', '')
keep = ['Package', 'Version', 'Architecture', 'Maintainer', 'Section', 'Priority', 'Description', 'Depends', 'Provides', 'Replaces', 'Conflicts', 'Installed-Size', 'Homepage']
lines = []
for k in keep:
    if k in fields:
        lines.append(f'{k}: {fields[k]}')
if 'Status' not in fields:
    lines.append('Status: install ok installed')
stanza = '\n'.join(lines)
pattern = re.compile(r'(?ms)^Package:\s*' + re.escape(pkg) + r'\s*$.*?(?=^Package:|\Z)')
existing = pattern.sub('', existing)
existing = existing.rstrip('\n') + '\n\n' + stanza + '\n'
open(status_path, 'w', encoding='utf-8').write(existing)
PY
	fi
	echo "    done"
	rm -rf "$tmp"
	added=$((added + 1))
done

# Rootless packages put their applications in the prefix (/var/jb/Applications).
# Copy them to /Applications as well, where LaunchServices finds them at boot
# (userland/build.sh does the same for the prefix at build time).
if [ -d system-tree/jb/Applications ]; then
	for app in system-tree/jb/Applications/*.app; do
		[ -d "$app" ] || continue
		name=$(basename "$app")
		rm -rf "system-tree/Applications/$name"
		cp -a "$app" "system-tree/Applications/$name"
		echo "  copied $name to /Applications"
	done
fi

if [ "$added" -eq 0 ] && [ "$REBUILD_ONLY" -eq 0 ]; then
	echo "add-deb.sh: nothing added ($skipped skipped)" >&2
	exit 1
fi

if [ "$DRY" -eq 1 ]; then
	echo "==> dry run: would rebuild the system image and write $IMAGE"
	exit 0
fi

echo "==> rebuilding the system image"
if ! python3 "$REPO/tools/apfs/mkapfs.py" -z -o system-new.apfs System=system-tree > mkfs.log 2>&1; then
	echo "add-deb.sh: mkapfs.py failed; see $WORK/mkfs.log" >&2
	tail -20 mkfs.log >&2
	exit 1
fi
if [ "${CHECK:-0}" = 1 ] && [ -x "$REPO/tools/apfs/check-image.sh" ]; then
	"$REPO/tools/apfs/check-image.sh" system-new.apfs System=system-tree || { echo "add-deb.sh: check failed" >&2; exit 1; }
fi
cat flashboot-split.apfs system-new.apfs > ram-resident-new.apfs || exit 1

if [ "$BACKUP" -eq 1 ] && [ -f "$IMAGE" ]; then
	free=$(df -B1 --output=avail "$(dirname "$IMAGE")" 2>/dev/null | tail -1)
	size=$(stat -c %s "$IMAGE")
	if [ -n "$free" ] && [ "$free" -lt "$size" ]; then
		echo "add-deb.sh: not enough free space for a backup; skipping it" >&2
	else
		echo "==> backup: $IMAGE.bak"
		cp -f "$IMAGE" "$IMAGE.bak" || echo "add-deb.sh: backup failed" >&2
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
