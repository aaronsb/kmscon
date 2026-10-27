"""The implementation under test: the C reference's packages, or a vtium
build given as an install tree or as package files.

Every variant of a run installs the same implementation from its disc
(guest/setup): packages through pacman, a tree as tree.tar unpacked over the
guest's root. Its size on disk is measured here, on the host, the same way for
both: the bytes of the regular files it installs, which for a package is
the size its .PKGINFO records.
"""

import hashlib
import os
import tarfile

from t3_common import RUNDIR, rel

C, VTIUM = "c", "vtium"
# What the guest needs of a vtium install (README, "vtium through the same
# harness"): the VT unit the base image's autovt@.service links to.
VT_UNIT = "usr/lib/systemd/system/kmsconvt@.service"
# The package metadata at the root of a package archive, which pacman reads
# and does not install.
PKG_META = {".PKGINFO", ".BUILDINFO", ".MTREE", ".INSTALL", ".CHANGELOG"}


def c_reference(pkgs):
    """The C reference's packages, as `t3_scenario.newest_packages` found them."""
    return _impl(C, "packages", pkgs)


def vtium(paths, tar_path=None):
    """A vtium build: one install tree (a directory laid out as the guest's
    root), or one or more package files. A tree is packed into `tar_path`
    (default fork/build/tier3/vtium-tree.tar) for the disc, owned by root."""
    paths = [os.path.abspath(p) for p in paths]
    if len(paths) == 1 and os.path.isdir(paths[0]):
        impl = _impl(VTIUM, "tree", paths)
        impl.update(pack_tree(paths[0], tar_path or os.path.join(RUNDIR, "vtium-tree.tar")))
        return impl
    for p in paths:
        if os.path.isdir(p) or not p.endswith(".pkg.tar.zst"):
            raise SystemExit("--vtium takes one install tree or package files (.pkg.tar.zst); "
                             "%s is neither" % p)
        if not os.path.isfile(p):
            raise SystemExit("no vtium package at %s" % p)
    return _impl(VTIUM, "packages", sorted(paths))


def _impl(name, kind, paths):
    files = [contents(kind, p) for p in paths]
    if name == VTIUM and not any(VT_UNIT in f for f in files):
        raise SystemExit("the vtium build at %s installs no /%s, which the guest starts on "
                         "each VT" % (", ".join(map(rel, paths)), VT_UNIT))
    sizes = [sum(f.values()) for f in files]
    return {"name": name, "kind": kind, "paths": paths,
            "bytes": sizes, "installed_bytes": sum(sizes)}


def contents(kind, path):
    """{installed path: bytes} of every regular file a package or tree
    installs, paths relative to the root, without a leading "./"."""
    out = {}
    if kind == "tree":
        for d, dirs, names in os.walk(path):
            dirs.sort()
            for n in names:
                full = os.path.join(d, n)
                if os.path.isfile(full) and not os.path.islink(full):
                    out[os.path.relpath(full, path)] = os.lstat(full).st_size
        return out
    with tarfile.open(path, "r:zst") as t:
        for m in t:
            name = m.name.removeprefix("./")
            if m.isfile() and name not in PKG_META:
                out[name] = m.size
    return out


def pack_tree(tree, tar_path):
    """Pack `tree` into `tar_path`, every entry owned by root, in sorted order,
    so the same tree gives the same archive. Returns its "tar" path and
    "tree_sha256", which the guest reports back from the disc."""
    def as_root(info):
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        return info

    os.makedirs(os.path.dirname(tar_path), exist_ok=True)
    with tarfile.open(tar_path, "w", format=tarfile.PAX_FORMAT) as t:
        for name in sorted(os.listdir(tree)):
            t.add(os.path.join(tree, name), arcname=name, filter=as_root)
    h = hashlib.sha256()
    with open(tar_path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return {"tar": tar_path, "tree_sha256": h.hexdigest()}


def packages(impl):
    """The package files the guest installs with pacman; none for a tree."""
    return impl["paths"] if impl["kind"] == "packages" else []


def describe(impl):
    """The report's account of the implementation: which ran, from where, and
    its size on disk in bytes, by path and in all."""
    out = {"name": impl["name"], "kind": impl["kind"],
           "paths": [{"path": rel(p), "bytes": b} for p, b in zip(impl["paths"], impl["bytes"])],
           "installed_bytes": impl["installed_bytes"]}
    if impl["kind"] == "tree":
        out["tree_sha256"] = impl["tree_sha256"]
    return out


def mib(nbytes):
    """`nbytes` in MiB (1 MiB is 1,024 KiB), to two places, or "none"."""
    return "none" if nbytes is None else "%.2f MiB" % (nbytes / (1 << 20))
