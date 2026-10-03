#!/usr/bin/env python3
"""Regenerate a package manifest from the import.json of a verified image import.

    adimport --image <the package's image> --dest <scratch>
    python importer/gen_known_files.py <scratch>/win/import.json                    (Deluxe: version 1)
    python importer/gen_known_files.py <scratch>/win/packages/<id>/import.json      (any other package: version 2)

(for a release on several install disks, every disk's image: --image <disk 1> --image <disk 2> ...,
or the ZIP they came in, or -- The Far Side -- the ZIP of each disk's files the same way; for a
release known by the ZIP of its install files -- Marvel Comics Screen Posters, Snoopy's Screen
Savers, the Looney Tunes, ScreamSavers, the Disney Collection, Dilbert -- that ZIP: --image <the ZIP>)

writes known_files.inc (Deluxe) or known_files_<id>.inc next to this script (for another build of a
release with several builds -- the Opus 'n Bill Screen Saver's November 1993 disks, The Flintstones'
May 1994 disks -- import.json's package.build names it, and the manifest is
known_files_<id>_<build>.inc).

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
    # The CD.
    "tng": (("0b95b9271c75b9ff1d89b57a0e15ee7b",), "packages/tng", ("ST-TNG", "ENGINE")),
    # The floppy (its image in the KryoFlux dump's ZIP). No ENGINE: the program is its own.
    "castaway": (("81087ea7cc6a304896e81c722b0a85ec",), "packages/castaway", ("SCRANTIC",)),
    # Three install floppies, known by a ZIP of each disk's files (three BBS copies; DISKS below).
    "opus": ((), "packages/opus", ("SAVER", "ENGINE")),
    # The ZIP of the four floppies' files, in one folder.
    "opusroad": (("ad6023bae1deb7c55c8239d55d1a81cf",), "packages/opusroad", ("SAVER", "ENGINE")),
    # Three install floppies, known by a ZIP of each disk's files (DISKS below).
    "flintstones": ((), "packages/flintstones", ("SAVER", "ENGINE")),
    # Three install floppies' images (DISKS below; the ZIP they came in holds them).
    "intermission": ((), "packages/intermission", ("SAVER", "ENGINE")),
}

# packages.cc: the build the package's own fields describe (Package::build),
# for a release with several builds.
PRIMARY_BUILDS = {"opus": "1993-09", "flintstones": "1994-06"}

# packages.cc: the known images of a release's other builds (Package::builds),
# (package, build) -> {md5: disk}; the manifest is known_files_<id>_<build>.inc.
BUILDS = {
    # The Opus 'n Bill Screen Saver's November 1993 build: a 1993 BBS copy, a
    # ZIP of each disk's files (WC!OPUS1..3.ZIP).
    ("opus", "1993-11"): {
        "a90d7e7545afb1020a1781f006dcbc0f": 1,
        "95dc225cd0aeb5e2976253af2ffbe05e": 2,
        "a2f001b54cd03059d154eead871ee87c": 3,
    },
    # The Flintstones' May 1994 build: a 1994 BBS copy (FLINT1..3.ZIP).
    ("flintstones", "1994-05"): {
        "68cf70016ec2438f2773678d2864e48c": 1,
        "9b5faa07d55bbdb9a848645a6157ae05": 2,
        "f6f94599e24d639fbdc5e2408a9c5c79": 3,
    },
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
    # The Opus 'n Bill Screen Saver's three floppies as three 1993 BBS copies,
    # a ZIP of each disk's files (OPUS1..3NTA.ZIP, ONBSBS-1..3.ZIP, OPUS-1..3.ZIP).
    "2aed4db32e141babe6a4336aac674cd2": ("opus", 1),
    "10cdbe9634bf25292ad331dbbe5f7905": ("opus", 2),
    "54b35b007c9ed7155441eb79de49d580": ("opus", 3),
    "30c17d0fc45cb8b286d80e79e68bb42e": ("opus", 1),
    "d5face113998cc8dc00db29877dcfd97": ("opus", 2),
    "3e8946d995b84c0bd1d643fb0f018cc0": ("opus", 3),
    "ec56dca1d8d324e5fea121ad1f69b51f": ("opus", 1),
    "f3eaa8e4f8c80a9c41526211336e7a04": ("opus", 2),
    "6441ea716809414208ee1b45ee13836a": ("opus", 3),
    # The Flintstones' June 1994 build's three floppies as a 1994 BBS copy
    # (FLINTST1..3.ZIP).
    "a53599e3a8c1fbfed7147ef769c67d53": ("flintstones", 1),
    "71a588d1f54f85f3a58326b16f0ad33b": ("flintstones", 2),
    "d43fa04cda64a0f158eb84b0d96f6f24": ("flintstones", 3),
    # Intermission 4.0's three floppy images (ITM4W-D1..3.IMA).
    "fc1305b7f178adf862bebde610f51020": ("intermission", 1),
    "f1fedb8dbd9fdde088b617cc2e543740": ("intermission", 2),
    "a56edc671fe824830c4b6aa08ad5c57d": ("intermission", 3),
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
    # A release with several builds: the build import.json records picks the
    # known images (another build's are its own) and the manifest's name.
    build = j.get("package", {}).get("build") if j.get("version") == 2 else None
    if build != PRIMARY_BUILDS.get(pid):
        if (pid, build) not in BUILDS:
            sys.exit("refusing: import.json's build %r is no known build of %s" % (build, pid))
        known_md5s, disks = (), dict(BUILDS[(pid, build)])
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
    other_build = build is not None and build != PRIMARY_BUILDS.get(pid)
    name = ("known_files.inc" if pid == "deluxe" else
            "known_files_%s_%s.inc" % (pid, build) if other_build else "known_files_%s.inc" % pid)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    with open(out, "w", encoding="utf-8", newline="\n") as w:
        if pid == "deluxe":
            w.write(DELUXE_HEADER % image_md5)
        elif disks:
            what = "%s, build %s" % (title, build) if build else title
            w.write(DISK_SET_HEADER % (pid, what, "ZIPs" if src.get("format") == "zip" else "images", image_md5))
        elif src.get("format") == "zip":
            w.write(ZIP_HEADER % (pid, title, image_md5))
        else:
            w.write(PACKAGE_HEADER % (pid, title, image_md5))
        for f in files:
            w.write('{"%s", %d, "%s"},\n' % (f["path"], f["size"], f["md5"]))
    print("wrote %d entries to %s" % (len(files), out))


if __name__ == "__main__":
    main()
