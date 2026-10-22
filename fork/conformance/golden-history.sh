#!/bin/sh
# Goldens changed since the previous ledger run, for the run page.
#
#   golden-history.sh LEDGER OUTDIR
#
# Reads the kmscon and libtsm commits of LEDGER's last row and writes
# OUTDIR/changes.tsv: a "previous<TAB>kmscon<TAB>libtsm" line, then one
# "repo<TAB>status<TAB>path" line per golden or tolerance sidecar added (A),
# changed (M) or deleted (D) since, and a "repo<TAB>?<TAB>why" line for a
# repository whose history cannot be read. The previous copy of each changed
# or deleted file goes to OUTDIR/<repo>/<path>.
#
# Runs on the host: a worktree's .git does not resolve inside the dev image.
# kmscon is compared against its working tree, so a preview shows goldens
# not yet committed, untracked ones included; libtsm against the
# subproject's HEAD. The subproject is
# a shallow clone, so the previous libtsm commit, named in full by the
# libtsm.wrap of the previous kmscon commit, is fetched when it is missing.
#
# SPDX-License-Identifier: MIT

set -eu
ledger=$1
out=$2
rm -rf "$out"
mkdir -p "$out"
tsv=$out/changes.tsv
: > "$tsv"

# A note may hold an escaped \|, which is no column boundary.
row=$(grep '^| [0-9]' "$ledger" | tail -n 1 | sed 's/\\|//g' || true)
[ -n "$row" ] || exit 0
pk=$(printf '%s\n' "$row" | awk -F'|' '{gsub(/[ `]/, "", $4); print $4}')
pl=$(printf '%s\n' "$row" | awk -F'|' '{gsub(/[ `]/, "", $5); print $5}')
printf 'previous\t%s\t%s\n' "$pk" "$pl" >> "$tsv"

# changes REPO GITDIR FROM TO PATH...: one line per changed file, and the
# previous copy of each modified or deleted one. An empty TO is the working
# tree. A git command that fails marks the repository's history unreadable
# rather than reading as no change.
changes() {
	repo=$1 dir=$2 from=$3 to=$4
	shift 4
	list=$out/$repo.list new=$out/$repo.untracked
	: > "$new"
	if ! git -C "$dir" diff --no-renames --name-status "$from" ${to:+"$to"} -- "$@" > "$list" ||
	   { [ -z "$to" ] && ! git -C "$dir" ls-files --others --exclude-standard -- "$@" > "$new"; }; then
		printf '%s\t?\tgit cannot list the changes since %s\n' "$repo" "$from" >> "$tsv"
		rm -f "$list" "$new"
		return 0
	fi
	awk '{ print "A\t" $0 }' "$new" >> "$list"
	rm -f "$new"
	while IFS="$(printf '\t')" read -r status path; do
		printf '%s\t%s\t%s\n' "$repo" "$status" "$path" >> "$tsv"
		case $status in
		M*|T*|D*)
			mkdir -p "$out/$repo/$(dirname "$path")"
			git -C "$dir" show "$from:$path" > "$out/$repo/$path" ;;
		esac
	done < "$list"
	rm -f "$list"
}

if git cat-file -e "$pk^{commit}" 2>/dev/null; then
	changes kmscon . "$pk" "" tests/goldens/tier2 fork/tier3/golden
else
	printf 'kmscon\t?\tcommit %s not found\n' "$pk" >> "$tsv"
fi

lt=subprojects/libtsm
full=$(git show "$pk:subprojects/libtsm.wrap" 2>/dev/null | sed -n 's/^revision *= *//p')
if [ ! -d "$lt/.git" ] && [ ! -f "$lt/.git" ]; then
	printf 'libtsm\t?\t%s is not a git checkout\n' "$lt" >> "$tsv"
elif [ -z "$full" ] || [ "${full#"$pl"}" = "$full" ]; then
	printf 'libtsm\t?\tthe libtsm.wrap of %s does not name %s\n' "$pk" "$pl" >> "$tsv"
elif ! git -C "$lt" cat-file -e "$full^{commit}" 2>/dev/null &&
     ! git -C "$lt" fetch --quiet --depth=1 origin "$full" 2>/dev/null; then
	printf 'libtsm\t?\tcannot fetch %s\n' "$pl" >> "$tsv"
else
	changes libtsm "$lt" "$full" HEAD test/conformance/golden
fi
