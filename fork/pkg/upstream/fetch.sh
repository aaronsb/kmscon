#!/bin/sh
# Check out URL at REV in DIR, for `make upstream-src` (fork/upstream.mk).
# Clones on first use, with pushing disabled. A branch REV is resolved against
# the remote's branch after a fetch, never a stale local one; a tag or commit
# is fetched only when it is not there yet. Refuses a checkout with local
# changes or untracked files, since the package is built from the tree as it
# stands. Left out of that check: subprojects/libtsm, which is a checkout of
# its own and checked on its own, and the hash file meson writes into a
# wrap's checkout.
#
#   fetch.sh DIR URL REV
set -eu
dir=$1 url=$2 rev=$3
if [ ! -e "$dir/.git" ]; then
    git clone -q -c remote.origin.pushurl=no-push "$url" "$dir"
fi
if git -C "$dir" ls-remote -q --exit-code --heads origin "$rev" >/dev/null; then
    git -C "$dir" fetch -q --tags origin
    want=$(git -C "$dir" rev-parse --verify "refs/remotes/origin/$rev^{commit}")
else
    if ! git -C "$dir" rev-parse -q --verify "$rev^{commit}" >/dev/null; then
        git -C "$dir" fetch -q --tags origin
    fi
    want=$(git -C "$dir" rev-parse --verify "$rev^{commit}")
fi
if [ -n "$(git -C "$dir" status --porcelain --untracked-files=all -- . \
        ':(exclude)subprojects/libtsm' ':(exclude).meson-subproject-wrap-hash.txt')" ]; then
    echo "fetch.sh: $dir has local changes or untracked files; remove it to fetch again" >&2
    exit 1
fi
git -C "$dir" -c advice.detachedHead=false checkout -q --detach "$want"
test "$(git -C "$dir" rev-parse HEAD)" = "$want"
echo "$dir: $url at $want"
