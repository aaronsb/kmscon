#!/bin/sh
# Check out URL at REV in DIR, for `make upstream-src` (fork/upstream.mk).
# Clones on first use, with pushing disabled, and fetches only when REV is not
# there yet. Refuses a checkout with local changes, since the package is built
# from the tree as it stands.
#
#   fetch.sh DIR URL REV
set -eu
dir=$1 url=$2 rev=$3
if [ ! -e "$dir/.git" ]; then
    git clone -q -c remote.origin.pushurl=no-push "$url" "$dir"
fi
if ! git -C "$dir" rev-parse -q --verify "$rev^{commit}" >/dev/null; then
    git -C "$dir" fetch -q --tags origin
fi
want=$(git -C "$dir" rev-parse --verify "$rev^{commit}")
if [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
    echo "fetch.sh: $dir has local changes; remove it to fetch again" >&2
    exit 1
fi
git -C "$dir" -c advice.detachedHead=false checkout -q --detach "$want"
test "$(git -C "$dir" rev-parse HEAD)" = "$want"
echo "$dir: $url at $want"
