#!/usr/bin/env python3
"""Regenerate a package manifest from the import.json of a verified image import.

    adimport --image <the package's image> --dest <scratch>
    python importer/gen_known_files.py <scratch>/win/import.json                    (Deluxe: version 1)
    python importer/gen_known_files.py <scratch>/win/packages/<id>/import.json      (any other package: version 2)

(for a release on several install disks, every disk's image: --image <disk 1> --image <disk 2> ...,
or the ZIP they came in, or -- The Far Side -- the ZIP of each disk's files the same way; for a
release known by the ZIP of its install files -- Marvel Comics Screen Posters, Snoopy's Screen
Savers, the Looney Tunes, ScreamSavers, the Disney Collection, Dilbert -- that ZIP: --image <the ZIP>)

writes known_files.inc (Deluxe) or known_files_<id>.inc next to this script.

A manifest is what lets adimport verify a folder source (a mounted disc, a
copy of one, copied floppies) file by file, where there is no image to hash.
Only an import whose image md5 is one of the package's known images -- or,
for a release on several install disks, whose images' md5s are every one of
its known disks exactly once -- is accepted as the reference, so a manifest
can never be seeded from a damaged or different pressing. It holds paths, sizes and md5s only -- never bytes of
the release, and never the AD 3.x archive password (import.json does not hold
it either).
"""
import json
import os
import re
import sys

# packages.cc: each package's known images (kXxxImages), its root and the
# folders under it.
PACKAGES = {
    "deluxe": (("d875a60338b73f44b7befa06bdd33aeb",), "FILES", ("AD40", "CLASSIC", "ENGINE", "AFI")),
    "ad10": (("a8d088415d391ce0d3199a831e1b2ad6",), "packages/ad10", ("AD10TH", "ENGINE", "AFI")),
    "ad32": (("8b8be6977375fbf4d54146b9d505aa1c",), "packages/ad32", ("AD32", "ENGINE")),
    "tt": (("541b9cfd744c377263a4ab1a144bdc12",), "packages/tt", ("TWISTED", "ENGINE")),
    "simpsons": (("7674adbda6fc5402d4e7a78af14880cc",), "packages/simpsons", ("SIMPSONS", "ENGINE")),
    # The CD, and the Redump BIN of the same pressing (the same files).
    "swse": (("bfa63c1bce15dcbea965dfd7c2ed44e8", "ce51614a3484b9269b5ed9e61510e971"), "packages/swse",
             ("SAVER", "ENGINE", "WINDOWS")),
    # Two install floppies (DISKS below): no image of the whole release.
    "startrek": ((), "packages/startrek", ("AFTERDRK", "ENGINE")),
    # No image of the floppies exists: the two ZIPs of their files (the flat one
    # of item afterdarkmarvelscreenposters; the after-dark-collection copy, in
    # Disk1 and Disk2 folders).
    "marvel": (("4c608dbbeb34108b30ede88304912c94", "6981b36abb04779a076466fabad3721c"), "packages/marvel",
               ("AFTERDRK", "ENGINE")),
    # No image of the floppies exists: the ZIP of their files (Disk1 and Disk2
    # folders). No ENGINE: the release ships none.
    "snoopy": (("a712447e1c957767bdbca884cead02dc",), "packages/snoopy", ("AFTERDRK",)),
    # The ZIP of the install files (the Internet Archive's after-dark-collection
    # copy), and the CD of the same files.
    "looney": (("642b358a4854c481fe99984b8452ceb5", "6ad72e19b2cf6fcb9e67427f8e600449"), "packages/looney",
               ("LNYTUNES", "ENGINE")),
    # No image of the floppies exists: the ZIP of their files (DISK1-DISK3 folders).
    "screams": (("37a47b25dd35b214f94f57b6a0c2bd02",), "packages/screams", ("SCREAMS", "ENGINE")),
    # No image of the floppies exists: the ZIP of their files.
    "disney": (("2f38df15494728b5bc20d26c36ba84c7",), "packages/disney", ("DISNEY", "ENGINE")),
    # Five install floppies, known by a ZIP of each disk's files (DISKS below).
    "farside": ((), "packages/farside", ("SAVER", "ENGINE")),
    # The flat ZIP of the four floppies' files, and (DISKS below) a ZIP of each disk's.
    "dilbert": (("ea6e18463d156fbb5c70401e39b45962",), "packages/dilbert", ("SAVER", "ENGINE")),
}

# packages.cc: the known images of releases on several install disks
# (KnownImage::disk): md5 -> (package, disk). The Internet Archive's two
# images of Star Trek: The Screen Saver, and the same disks as a Windows 9x
# copy wrote to them.
DISKS = {
    "28e33608b8d3bafa28585472c4a7a9ac": ("startrek", 1),
    "c630da5f6839303b599947f56fdd7c25": ("startrek", 2),
    "6ee71b45e32b07001d46ab8c80af589d": ("startrek", 1),
    "af9d29a7ddea2c03618899c1c5733c67": ("startrek", 2),
    # The Far Side Screen Saver Collection's five floppies as a 1994 BBS copy,
    # a ZIP of each disk's files (PNX-FSC1..5.ZIP).
    "bfbe487438204b3573e913a774ee964d": ("farside", 1),
    "364307265d011ba3e31d383bc178d56d": ("farside", 2),
    "5ac67d84f9a5262f40c92e9f143fdb03": ("farside", 3),
    "7902b2a2a89eb7ed391e179a27958061": ("farside", 4),
    "58351b2eedd8f6ef5ec51fa00c4d8d17": ("farside", 5),
    # Dilbert's four floppies the same way (DILBERT1..4.ZIP).
    "1158cc6333d3b22dcd401a0593006e6f": ("dilbert", 1),
    "4347386255e85cddb39a5d69ab65bc81": ("dilbert", 2),
    "0f5408c77ed018db8b99b6b70f2a6a29": ("dilbert", 3),
    "9064065cfb1edd12cc659823b5ca88ad": ("dilbert", 4),
}

DELUXE_HEADER = """\
// Manifest of ADE/FILES/{AD40,CLASSIC,ENGINE,AFI} on the After Dark 4.0 Deluxe
// CD (image md5 %s): {path relative to <assets>\\win, size, md5}.
// Sizes and hashes only -- no After Dark bytes. GENERATED by
// gen_known_files.py from the import.json of an import of that image; do not
// edit by hand.
"""

PACKAGE_HEADER = """\
// Manifest of package "%s" (%s) as adimport installs it from the image with
// md5 %s: {path relative to <assets>\\win, size, md5}, fix-up copies
// included. Sizes and hashes only -- no bytes of the release. GENERATED by
// gen_known_files.py from the import.json of an import of that image; do not
// edit by hand.
"""

ZIP_HEADER = """\
// Manifest of package "%s" (%s) as adimport installs it from the ZIP of its
// install files with md5 %s, one of its known images: {path relative to
// <assets>\\win, size, md5}. Sizes and hashes only -- no bytes of the release.
// GENERATED by gen_known_files.py from the import.json of an import of that
// ZIP; do not edit by hand.
"""

DISK_SET_HEADER = """\
// Manifest of package "%s" (%s) as adimport installs it from the %s
// of its install disks, with md5s
//   %s:
// {path relative to <assets>\\win, size, md5}. Sizes and hashes only -- no
// bytes of the release. GENERATED by gen_known_files.py from the import.json
// of an import of those images; do not edit by hand.
"""

# 8.3 names as the sources list them; the fix-up copies (PACKAGES.md §4.3)
# keep the long names the modules open ("Flying Toasters.mid").
COMPONENT = re.compile(r"[A-Za-z0-9_!#$%&'()@^`{}~. -]+")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_known_files.py <import.json of a verified image import>")
    with open(sys.argv[1], encoding="utf-8") as f:
        j = json.load(f)
    src = j.get("source", {})
    if j.get("version") == 1:
        pid = "deluxe"
        image_md5 = src.get("isoMd5")
        title = "After Dark 4.0 Deluxe"
    elif j.get("version") == 2:
        pkg = j.get("package", {})
        pid = pkg.get("id")
        image_md5 = src.get("imageMd5")
        title = pkg.get("title", pid)
        if pid not in PACKAGES or pid == "deluxe":
            sys.exit("refusing: unknown package %r" % pid)
        if pkg.get("root") != PACKAGES[pid][1]:
            sys.exit("refusing: package root %r, expected %r" % (pkg.get("root"), PACKAGES[pid][1]))
    else:
        sys.exit("refusing: import.json version %r" % j.get("version"))
    known_md5s, root, dirs = PACKAGES[pid]
    disks = {m: d for m, (p, d) in DISKS.items() if p == pid}
    # One known image of the whole release (a package known both ways, as
    # dilbert is, by either), else the set of its install disks.
    if image_md5 in known_md5s:
        disks = {}
    if disks:
        # Every install disk exactly once, and nothing else (either copy of a
        # disk alike): the parts of one import of the whole set.
        parts = [p.get("md5") for p in src.get("parts", [])]
        given = sorted(disks.get(m, 0) for m in parts)
        if image_md5 is not None:
            sys.exit("refusing: import.json has one image (md5 %s); %s came on install disks" % (image_md5, pid))
        if given != list(range(1, max(disks.values()) + 1)):
            sys.exit("refusing: import.json's images (%s) are not every known %s install disk once (%s)" %
                     (", ".join(map(str, parts)) or "none", pid, ", ".join(sorted(disks))))
        image_md5 = " + ".join(sorted(parts, key=lambda m: disks[m]))
    elif image_md5 not in known_md5s:
        sys.exit("refusing: import.json's image md5 is %r, not a known %s image (%s)" %
                 (image_md5, pid, ", ".join(known_md5s)))
    if j.get("verified") != "image":
        sys.exit("refusing: import.json says verified %r, not \"image\"" % j.get("verified"))
    files = sorted(j["files"], key=lambda f: f["path"])
    seen = set()
    for f in files:
        p = f["path"]
        parts = p.split("/")
        prefix = root.split("/")
        if parts[:len(prefix)] != prefix or len(parts) < len(prefix) + 2 or parts[len(prefix)] not in dirs:
            sys.exit("unexpected path %r" % p)
        for c in parts[len(prefix):]:
            if c in (".", "..") or not COMPONENT.fullmatch(c) or c.endswith((" ", ".")):
                sys.exit("unexpected path %r" % p)
        if not re.fullmatch(r"[0-9a-f]{32}", f["md5"]) or p.upper() in seen or not isinstance(f["size"], int):
            sys.exit("bad entry %r" % f)
        seen.add(p.upper())
    name = "known_files.inc" if pid == "deluxe" else "known_files_%s.inc" % pid
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    with open(out, "w", encoding="utf-8", newline="\n") as w:
        if pid == "deluxe":
            w.write(DELUXE_HEADER % image_md5)
        elif disks:
            w.write(DISK_SET_HEADER % (pid, title, "ZIPs" if src.get("format") == "zip" else "images", image_md5))
        elif src.get("format") == "zip":
            w.write(ZIP_HEADER % (pid, title, image_md5))
        else:
            w.write(PACKAGE_HEADER % (pid, title, image_md5))
        for f in files:
            w.write('{"%s", %d, "%s"},\n' % (f["path"], f["size"], f["md5"]))
    print("wrote %d entries to %s" % (len(files), out))


if __name__ == "__main__":
    main()
