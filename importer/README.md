# adw_import / adimport.exe

Puts the original Windows screen saver modules where the hosts read them
(DESIGN.md §6, §7; the full specification is `docs/PACKAGES.md`), and keeps
each release's box cover (DESIGN.md §9, `docs/COVERS.md` §2). It is Long
After Dark's importer, and it knows fourteen releases, the *packages* of its
built-in registry (`packages.h`): nine of Berkeley Systems' After Dark, the
oldest being Star Trek: The Screen Saver (1992, After Dark 2.0b); two of
other companies' modules for After Dark, Binary Software's ScreamSavers and
Image Smith's Snoopy's Screen Savers; and three whose modules run on
Delrina's Intermission engine, not After Dark's: LucasArts' Star Wars Screen
Entertainment (1994), and Delrina's own The Far Side Screen Saver Collection
and Scott Adams' Dilbert Screen Saver Collection (both 1994).

| id | Release | Medium | Recipe | Installs to (`<win>` = `<assets root>\win`) | Modules | Internet Archive copy |
|---|---|---|---|---|---|---|
| `deluxe` | After Dark 4.0 Deluxe | hybrid CD, plain files | `tree` | `FILES\{AD40,CLASSIC,ENGINE,AFI}` | 84 | the CD image, 381.7 MB |
| `ad10` | After Dark 10th Anniversary | hybrid CD, ISO-9660 + Joliet, plain files | `tree` | `packages\ad10\{AD10TH,ENGINE,AFI}` | 46 | the CD image, 143.3 MB |
| `ad32` | After Dark 3.2 | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\ad32\{AD32,ENGINE}` | 44 | the CD image, 58.8 MB (3 copies) |
| `tt` | Totally Twisted After Dark | hybrid CD, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\tt\{TWISTED,ENGINE}` | 13 | the CD image, 37.9 MB |
| `simpsons` | The Simpsons Screen Saver | two floppies (FAT12), InstallShield 2 + encrypted PKZIP | `ad3zip` | `packages\simpsons\{SIMPSONS,ENGINE}` | 15 | a ZIP of the install files, 2.6 MB (2 copies) |
| `swse` | Star Wars Screen Entertainment | CD copy of five install floppies (plain ISO-9660), Presage installer: multi-volume ARJ + SZDD | `intermission` | `packages\swse\{SAVER,ENGINE,WINDOWS}` | 14 | the CD image, 6.9 MB (3 copies: the ISO, the Redump BIN, a flat ZIP) |
| `startrek` | Star Trek: The Screen Saver | two 1.44 MB floppies (FAT12), Microsoft Setup 2.0: KWAJ-compressed files | `ad2kwaj` | `packages\startrek\{AFTERDRK,ENGINE}` | 16 | the two floppy images, 1.4 MB each (2 copies) |
| `marvel` | Marvel Comics Screen Posters | two floppies, InstallShield 2.00: compressed libraries, one split over both disks | `islib` | `packages\marvel\{AFTERDRK,ENGINE}` | 1 | a ZIP of the install files, 1.9 MB (2 copies: flat, and in `Disk1`/`Disk2` folders) |
| `snoopy` | Snoopy's Screen Savers | two floppies, InstallShield 2.00: one compressed library split over both | `islib` | `packages\snoopy\AFTERDRK` (no `ENGINE`) | 8 | a ZIP of the install files in `Disk1`/`Disk2` folders, 1.9 MB |
| `looney` | The Looney Tunes Screen Saver | two floppies or their CD copy, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\looney\{LNYTUNES,ENGINE}` | 12 | a ZIP of the install files, 2.8 MB |
| `screams` | ScreamSavers | three floppies, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\screams\{SCREAMS,ENGINE}` | 15 | a ZIP of the install files in `DISK1`–`DISK3` folders, 3.3 MB |
| `disney` | The Disney Collection Screen Saver | three floppies, InstallShield 3 + encrypted PKZIP | `ad3zip` | `packages\disney\{DISNEY,ENGINE}` | 16 | a ZIP of the install files, 3.4 MB |
| `farside` | The Far Side Screen Saver Collection | five 1.44 MB floppies, Delrina's Intermission Installer: loose files, most SZDD | `intermission` | `packages\farside\{SAVER,ENGINE}` | 14 | a ZIP of each install disk's files, 5.5 MB in all (a 1994 bulletin-board copy, the only intact one) |
| `dilbert` | Scott Adams' Dilbert Screen Saver Collection | four 1.44 MB floppies, Delrina's Intermission Installer | `intermission` | `packages\dilbert\{SAVER,ENGINE}` | 16 | a ZIP of the install files, 4.3 MB (2 copies: flat, and a ZIP per disk) |

```
adimport --image <image> [--image <image2> …] | --iso <image> | --from <drive or folder>
         | --download [<id> | all]
         [--package <id>] [--dest <assets root>] [--gui] [--no-verify] [--quiet]
         [--download-dir <dir>] [--url <url> [--md5 <hex>]] [--no-cover-download]
adimport --catalog-only [--dest <assets root>] [--quiet]
adimport --list-packages [--dest <assets root>]
adimport --remove <id> [--dest <assets root>] [--quiet]
adimport --set-cover <id> <picture> [--dest <assets root>] [--quiet]
adimport --clear-cover <id> [--dest <assets root>] [--quiet]
adimport --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>] [--quiet]
adimport --gui --change-cover <id> [--dest <assets root>] [--download-dir <dir>] [--no-cover-download]
adimport --gui --refresh-covers [<id> | all] [--force] [--dest <assets root>] [--download-dir <dir>]
adimport --gui --remove <id> [--dest <assets root>]
```

Every import ends by rewriting `catalog-win.json` over every installed
package (see **Catalog**); `--catalog-only` imports nothing and rewrites it
from the files already there. `--list-packages` prints each registry
package, whether (and how verified) it is installed, and the size of its
download (every image of a floppy set: "download 2.8 MB (2 floppy
images)"; a ZIP of install files: "download 3.4 MB (ZIP of the install
files)"; a ZIP of each install disk's files: "download 5.5 MB (5 ZIPs of
the install disks' files)"), and its cover; its title column is as wide as
the longest title, "Scott Adams' Dilbert Screen Saver Collection" (44
characters). `--remove <id>`
deletes one package (Deluxe: `FILES` and its `import.json`) and rewrites the
catalog; with `--gui` a window asks first (the settings dialog's "Remove …"
and the importer's own covers run it: `gui/README.md`), exit 0 when it was
removed and 5 when not. The cover commands are described under **Covers**.

## Sources

Every source is read through one view, `SourceFs` (`source.h`):

| Source | What is read |
|---|---|
| `--image` (= `--iso`) | The type is sniffed from the content, never the extension. **ISO-9660** (`iso9660.h`): level 1/2, Joliet SVD preferred when present (each Joliet entry is paired with its 8.3 twin), multi-extent files, one-sided both-endian fields, cooked 2048 or raw 2352-byte sectors; the Apple partition map and HFS half of a hybrid disc are ignored. Otherwise, a local file header at byte 0: a ZIP (`zip.h`, held in memory, at most 256 MB). A ZIP that holds **floppy images** — members of a DOS floppy's size (a multiple of 512 bytes, 160 KB to 2.88 MB) that open as FAT volumes — is those images (`source.h` `floppy_images_in_zip`): each is inflated into memory with its size and CRC-32 checked and read as if it had been given with `--image`, a part of its own named `<zip>!<member>`; the other members (scans, metadata) are ignored and logged — a floppy-sized one whose first sector is no boot sector is not inflated past its first 64 KiB (the output chunk that holds that sector) — and a password-protected floppy-sized member is refused, as is a ZIP whose members with a boot sector add up to more than 64 MB (`kMaxZippedImageBytes`: "… holds more than 64 MB of disk images; no release came on that many disks"). That is how the Internet Archive serves an item's disks together (Star Trek's: the ZIP's own md5 changes with every download, so its images' md5s identify it). Member names are UTF-8 when the archive says so (general-purpose bit 11; each byte that is not UTF-8 becomes U+FFFD) or when they are UTF-8 without it (archivers outside Windows write that); any other name is code page 437, which the ZIP specification prescribes without the bit and which Explorer and 7-Zip on an English Windows write for a name that fits it (their OEM code page; byte 0x81 is `ü`), the same reading on every machine: so two names that differ only in a letter outside ASCII are two names, and nothing that is not UTF-8 reaches a message or `import.json`. Any other ZIP is a **ZIP of install files**, whose members are the files at the source's root — bare names only, or, since the twelve releases, nothing but flat `DISK<n>` folders (below; any other nesting is refused: "… (a ZIP source holds the install files at its root, or only DISK<n> folders)") — none password-protected (the installer's own encrypted archives are members like any file), each inflated and its size and CRC-32 checked as it is read. A ZIP of install files whose md5 is one of a package's known images (the five releases below that have no image of their disks online) is that release's known copy: read as the install folder, verified `image`, and logged "a ZIP of install files, the known copy of <title> (by its md5)". Otherwise **FAT12/16** (`fat.h`): BPB-driven, strict (see below). The image md5 is always computed. Several `--image`s (split floppies: the Simpsons' two, Star Trek's two, Star Wars Screen Entertainment's five; or ZIPs of each install disk's files: The Far Side's five, Dilbert's four), and the images of a ZIP, are read as one tree: directories merge; a file present in more than one image must have the same size (checked when listed) and bytes (checked when read), else the source is invalid ("… they are not the disks of one release"). An image given twice, or a byte-identical copy of one (same md5 and size), is read once and logged as ignored, so the import still counts as one known image. Known images of two different releases are refused before anything is read: "these images are two different releases (…); import each image on its own". |
| `--from` | A folder or drive root. The root of a **CD drive** (a disc, or an image Windows mounted) is read as the disc itself, through the ISO reader on the raw volume (`\\.\E:`, sector-aligned reads): Windows lists a Joliet disc such as the 10th Anniversary by its long names ("Toaster 2k.ad"), and only the disc's own pairing gives the 8.3 names the release, its manifest and the catalog ids use (`TOASTER2.AD`). When the volume cannot be opened it falls back to the listing (logged). Anything else: names are taken as listed, upper-cased (never the volume's generated `~1` alias, which depends on the drive's 8dot3name setting). A folder whose root holds nothing but `DISK<n>` folders is a disk set (below); one that holds anything else beside them is read as it is, and the log says why (so does the importer window's caution: `identify_folder` puts `open_folder`'s note before the error). |
| `--download` | The package's Internet Archive copy (see **Downloads**; Deluxe when neither `--download <id>` nor `--package` names one) via WinHTTP into `<data folder>\downloads` (see **Destination and atomicity**; or `--download-dir`): redirects followed by hand (never from https to http), resume from `<file>.part` with `Range`, restart when a server ignores or botches it, the published size checked before a byte is written and while it streams (a body with no `Content-Length` included; without a published size, 2 GB at most), and the md5 (CNG) before the rename. An already-downloaded file that matches is reused. Retries: 5 failed connections in a row, reset whenever a connection gets the file further (cap 50). One download per destination at a time (`<file>.lock`, delete-on-close). The downloaded file — or every image of a copy on several floppies, each fetched and checked the same way — is then read as `--image` would read it. `--url` fetches another URL instead (saved under its own decoded file name — `download.iso` when that is a DOS device such as `NUL.iso` — checked against `--md5` when given); `--md5` must be 32 hex digits. Without `--md5`, the URL is recorded in `<file>.source` (hidden) and the file, or its `.part`, is reused only for that same URL. `--download all`: every package in turn. A cancel (the window's Cancel, or Ctrl+C at the console) stops a download at once, even while it waits on the network. |

**Disk sets** (`source.h`). A source whose root holds nothing but folders
named `DISK<n>` (`DISK1`–`DISK99`, any case, no leading zero; `zip.h`
`disk_folder_number`) is a release's install disks kept apart, and is read
as the union of those folders in disk order, exactly as several `--image`s
are: folders merge, and a name on two disks must be one file (the same size
when listed, the same bytes when read), else the source is invalid: "SAME.TXT
differs between Disk1 and Disk2 (size); they are not the disks of one
release". Any subset of disks is accepted; the recipe says whether the
release is complete. The Internet Archive's copies of ScreamSavers
(`DISK1/`–`DISK3/`), Marvel Comics Screen Posters and Snoopy's Screen Savers
(`Disk1/`, `Disk2/`) are ZIPs of this shape (Snoopy's `DREAM.ON`, on both of
its disks, is the same bytes on each). The log says "reading <source> as
the union of its folders Disk1 and Disk2 (one install disk each)".
* **A ZIP** (`ZipArchive` with `ZipNames::disk_folders`): its disks are
  flat, its members `DISK<n>/<bare name>` and the folders' own entries
  (`ZipMember::disk`, `directory`, `file_name()`). Another folder
  (`__MACOSX/` among them), a deeper path, a folder entry that holds data,
  or a file at the root beside the disks refuses it (2). The installers'
  own archives inside a source keep the strict bare names
  (`ZipNames::bare`).
* **A folder, a CD drive read as its disc, an ISO or FAT image**: the same
  rule, and a folder disk may hold subfolders, which merge. A root that
  holds anything besides its `DISK<n>` folders (a `desktop.ini`, the
  downloaded ZIP beside its unzipped disks) is read as it is, the note
  saying so, and is then usually no known release; the importer window's
  caution gives that note before the error (`identify_folder`).
* `import.json`'s `from` names a file by its path in the union, without
  its disk (`AFI.ZIP!SCREAMS.AFI`).

## Downloads

Each registry package lists its Internet Archive copies (`packages.h`
`Download`: URL, file name, size, md5, kind), from the archive.org search
verified on 2026-09-26 (`research/win/pkg/sources/sources.json`; Star Wars
Screen Entertainment's on 2026-09-28, `research/win/pkg/swse/sources.json`;
Star Trek's on 2026-09-29, `research/win/pkg/startrek/importer/sources.json`,
and the five later releases' on 2026-09-29 and 30 (their surveys'
`research/win/pkg/<survey>/`, and `import.download_real` fetching them):
every URL answered 302 → 200 (Star Trek's and the five's a Range request:
302 → 206) with the stated size and `Accept-Ranges`, and each file's md5
was compared with the user's own copy; The Far Side's and Dilbert's on
2026-09-30, `research/win/pkg/farside/` and `dilbert/`, the files inside
another ZIP fetched whole, as below):

| id | Copies, in the order tried | Saved as | Size | md5 | Kind |
|---|---|---|---|---|---|
| `deluxe` | `after-dark-4-deluxe` | `After Dark 4.0 Deluxe (1996)(Berkeley Systems)[Mac-PC].iso` | 400234496 | `d875a603…` | image |
| `ad10` | `ad10th` (the only exact copy; darkened once, restored 2024-07-19) | `ad10th.iso` | 150228992 | `a8d08841…` | image |
| `ad32` | `after-dark-v3_2`, `berkeley-systems-after-dark-for-windows`, `3x-…-after-dark-3.2-nirvana` (uncurated, last) | `After Dark 3.2 (1995)(Berkeley Systems)[Mac-PC].iso` | 61693952 | `8b8be697…` | image |
| `tt` | `TTW320CD` | `TTW320CD.ISO` | 39784448 | `541b9cfd…` | image |
| `simpsons` | `SIMPSONS_WIN/SIMPSONS.zip`, then `AfterDarkSimpsons/After Dark - The Simpsons.zip` | the same names | 2752575 / 2752010 | `90a85bf6…` / `1d608334…` | zip |
| `swse` | `cd_AfterDark_Star_Wars_ScreenSaver_for_Win3.1/AfterDarkStarWars.iso` (the exact image), then `swse1/SWSE.bin` (the Redump dump of the same pressing, raw 2352-byte sectors), then `swse_20240317/SWSE.zip` (the disc's 40 files, flat) | `AfterDarkStarWars.iso`, `Star Wars - Screen Entertainment (USA).bin`, `SWSE.zip` | 7227392 / 9005808 / 7013137 | `bfa63c1b…` / `ce51614a…` / `c7a4c532…` | image / image / zip |
| `startrek` | `afterdark-20b_startrek/afterdark-20b_startrek_disk1.img` + `…_disk2.img` (the user's images), then `startrektosscreensaver1992win/startrek1.img` + `startrek2.img` (the same disks as a Windows 9x copy wrote to them: the same files, byte for byte) | the same names | 1474560 each | `28e33608…` + `c630da5f…` / `6ee71b45…` + `af9d29a7…` | image, two parts |
| `marvel` | `afterdarkmarvelscreenposters/After Dark - Marvel Screen Posters.zip` (the twelve install files, flat, without the previous owners' notes), then `after-dark-collection/After Dark - Marvel Comics.zip` (both disks in `Disk1`/`Disk2` folders: the user's copy) | the same names | 2039771 / 2046286 | `4c608dbb…` / `6981b36a…` | zip (known images) |
| `snoopy` | `after-dark-collection/After Dark - Snoopy.zip` (`Disk1`/`Disk2` folders) | the same name | 1993700 | `a712447e…` | zip (a known image) |
| `looney` | `after-dark-collection/After Dark - Looney Tunes.zip` | the same name | 2900525 | `642b358a…` | zip (a known image) |
| `screams` | `after-dark-collection/After Dark - Scream Savers.zip` (`DISK1`–`DISK3` folders; the only copy online) | the same name | 3453163 | `37a47b25…` | zip (a known image) |
| `disney` | `after-dark-collection/After Dark - Disney Collection.zip` | the same name | 3560012 | `2f38df15…` | zip (a known image) |
| `farside` | `prog47_55/prog47_55.zip/prog47_55%2FPROG_52%2FPNX-FSC1.ZIP` … `PNX-FSC5.ZIP` (a 1994 bulletin-board copy of the five floppies, one ZIP per disk, inside the item's ZIP; the only intact copy online) | `PNX-FSC1.ZIP` … `PNX-FSC5.ZIP` | 977668 + 1210777 + 1202730 + 1106810 + 1248997 | `bfbe4874…` + `36430726…` + `5ac67d84…` + `7902b2a2…` + `58351b2e…` | zip, five parts (known images) |
| `dilbert` | `dilbert_screensaver_collection/DilbertS.zip` (the four floppies' files, flat), then `prog70_75/prog70_75.zip/prog70_75%2FPROG_70%2FDILBERT1.ZIP` … `DILBERT4.ZIP` (the same disks, one ZIP per disk, inside the item's ZIP) | the same names | 4511167 / 1077844 + 1146156 + 1193935 + 1155239 | `ea6e1846…` / `1158cc63…` + `43473862…` + `0f5408c7…` + `9064065c…` | zip / zip, four parts (known images) |

* **Disc images** (`kind` image): the md5 is one of the package's known
  images, so the import is exactly an `--image` import of that file:
  `verified: image`. Star Wars Screen Entertainment has two: the ISO, and the
  Redump BIN of the same pressing (its first 3529 cooked sectors are that ISO;
  the ISO reader takes raw sectors anyway).
* **The Simpsons** has no image of its floppies online (the known image is
  the owner's own merge of both disks, with their notes, so nothing can
  match its md5). Its copies are flat ZIPs of the 28 install files, every
  one md5-identical to the floppies' (the readme `CHANGES.TXT`, which the
  recipe never reads, is missing). The ZIP is checked against its published
  md5, read as the install folder (see `--image` above) and every installed
  file is verified against the manifest: `verified: files`. Star Wars Screen
  Entertainment's third copy is such a ZIP too (all 40 files of the disc).
* **Known ZIPs** (Marvel, Snoopy, the Looney Tunes, ScreamSavers, the Disney
  Collection): no image of their disks exists online (the Looney Tunes'
  CD, `LOONEY_T`, is online only under a name that carries the product's
  serial number, so it is a known image by md5, size and volume id alone,
  and never a download), and each copy's bytes never change, so each ZIP's
  md5 is a known image: the download is read as the install folder and
  verified `image`, as the user's own copy of the same file is. No
  serial-number file of the items is ever fetched.
* **Floppy sets** (Star Trek: The Screen Saver): a copy is the images of
  every install disk (`Download::more_images`, each a `DownloadPart`),
  fetched one after the other into the downloads folder, each checked
  against its own published size and md5, with the progress running over
  the whole set (`download_size`). A copy is used only when every image
  verifies; the images are then imported as `--image <disk 1> --image
  <disk 2>` would be: a known disk set, `verified: image`. `--md5` replaces
  the first image's md5 (and size) only, and `import.json`'s `url` and
  `finalUrl` are the first image's.
* **A ZIP per install disk** (The Far Side, and Dilbert's second copy): a
  `zip` copy may have parts too (`Download::more_images`), each disk's ZIP,
  fetched and checked the same way and then imported as several `--image`s
  are: a known disk set, `verified: image`, the log saying "ZIPs of the
  install disks' files, the known copies of <title> (by their md5s)". The
  Internet Archive serves each from inside the item's ZIP, by its path,
  with HTTP 200, no length up front and no `Range` support, so an
  interrupted one is fetched whole again. Their md5s are ours: the Internet
  Archive publishes none for a file inside a ZIP. These copies are 1994
  bulletin boards' repacks, with the boards' notes beside the release's
  files (`FILE_ID.DIZ`, `.NFO` and text files): the recipe opens only the
  files it installs, so the notes are never read.
* **Copies are tried in order.** One whose files are all already complete
  in the downloads folder goes first, so nothing is fetched for a package
  downloaded before, from whichever copy. When a copy, or any image of
  one, cannot be fetched (network: 404, DNS, …) or is not the published
  file (a server announcing
  another size is refused before a byte is written; a wrong md5 deletes the
  file), the next copy is tried; cancel, local I/O and the download lock end
  it at once. Copies with the same bytes share a file name, so a transfer
  interrupted on one resumes from the next. When every copy fails the
  import fails with 3 when one delivered a wrong file, else 4, and the
  message says to import from the disc with `--image` or `--from` (the
  Internet Archive does withdraw items). A different file is never accepted.
* `--download all` imports every package in registry order, each as its own
  `--download <id>` (going on after a failure, stopping at a cancel); the
  exit code is the first failure's, 0 when all were imported.
* A download's progress (`Progress::Phase::download`, then `check_image`
  while its md5 is checked: a reused file's, or the finished transfer's)
  names the file it is saved as (`Progress::item`: "After Dark -
  Snoopy.zip"), as a copy names the file it copies.

Names that could escape the staging directory or are DOS devices (`CON`,
`NUL.AD`, `COM1`…) reject the source, from every reader (`names.h`). So do
two names Windows takes for one file — two members of one archive, or two
planned files from anywhere — compared as the file system and
`CompareStringOrdinal` compare them, letters outside ASCII included (code
page 437's `ü`/`Ü` are one name): the source is refused (2) while it is
planned, never when the second file cannot be created.

**The staging budget.** A folder that the source lists under two names (an
ISO directory record, a FAT subdirectory or, in a folder, a junction that
reaches a directory already walked: `SourceFs::dir_key`) rejects the source,
as does nesting deeper than 16. An import plans at most 20,000 files and
2 GB (`ImportOptions::max_files`, `max_bytes`; the largest release is 175
files, 45 MB), and a planned file whose size differs from the manifest's
fails verification before anything is written (`--no-verify` imports it), so
a damaged or crafted source cannot fill the disk with copies before it is
checked.

**ARJ** (`arj.h`; Star Wars Screen Entertainment's `SWSE1.ARJ` and the
four-volume `SWSE2.ARJ` + `.A01`–`.A03`). The volumes are held in memory
(at most 64 MB each). Only what ARJ 2.x writes is accepted: a main header at
byte 0, local headers one after another, each followed by exactly its data,
and the end marker (anything after it is never looked at); every basic and
extended header's CRC-32 checked, sizes 30–2600, no more than 16 extended
headers, minimum version ≤ 3, flags among VOLUME, EXTFILE, PATHSYM and BACKUP
(garbled, password-protected archives are refused by name), file type 0 or 1,
methods 0–4, bare member names that pass the name rules below, at most 65,535
members. A member that spans volumes is its segments: the last member of one
volume carries VOLUME_FLAG and the first member of the next continues it
(EXTFILE_FLAG, the position it resumes at); every rule of that is checked
(the volumes' own flags, names, positions, order), and each segment is
decoded on its own. The decoder of methods 1–4 and its bit reader are a
modified version of UNARJ's (`DECODE.C` and the bit reader of `UNARJ.C`,
© 1991–93 Robert K. Jung / ARJ Software; its LZH routines derive from
Haruhiko Okumura's ar002), ported to C++ with every bound UNARJ lacks. UNARJ
may be used only in programs that are not ARJ archivers, so no program that
links it compresses ARJ: the tests carry fixed vectors, not an encoder.
Methods 1–3 are one LZH decoder: table sizes, code lengths (≤ 16), zero
runs and constant symbols are checked before use, a Huffman table must be a
complete code (Kraft sum exactly 1), a match may not reach before the start
or beyond the 26,624-byte window, no stream may produce more than its
recorded size, and the look-ahead may read at most 2 bytes past a segment.
When the recorded size is reached, a block may not still hold symbols;
what follows it (further blocks, trailing bytes) is never read, as UNARJ
never reads it. Method 4 ("fastest") has the same output and window bounds
and simply stops at the recorded size. Each segment's size and CRC-32 (over
the bytes produced) are checked as it streams (ARJ stores no whole-file
CRC). A damaged member is a corrupt source (2), never a verify failure.

**SZDD** (`szdd.h`; Microsoft COMPRESS 'A', the `*.XX_` files): the magic,
mode 'A' (KWAJ and other modes are refused), the expanded size; LZSS over a
4096-byte window that starts as spaces, written from 0xFF0 (Windows'
EXPAND.EXE gives the same bytes). Strict: the data must produce exactly the
header's size, no match may pass it, no byte may be left over, but for
Delrina's version stamps: The Far Side's and Dilbert's compressed libraries
end with one or two 8-byte records, `DLL ` and four digits (`DLL 0401`),
which its installer compared so that an older library never replaced a
newer one; they are dropped, and any other leftover byte, or a record that
is no stamp, is still refused. SZDD has no
checksum: a damaged SZDD file that still expands to its size is caught only
by the manifest (3), and not at all under `--no-verify`.

**KWAJ** (`kwaj.h`; the other format of Microsoft's COMPRESS, which
Microsoft Setup 2.0 expanded on the way: Star Trek: The Screen Saver's
`*.XX_` files). Only what that release uses is read, strictly: method 3 (LZ +
Huffman) with no header flags, so the data starts right after the 14-byte
header; another method, any flag, another data offset and an SZDD file are
refused by name. Written from the public format description (the research
reference `research/win/pkg/startrek/tools/kwaj.py`); no third-party code.
Five canonical Huffman codes (MATCHLEN, MATCHLEN2 after a literal run shorter
than 32, LITLEN, OFFSET, LITERAL), each in one of four length encodings
(table types 0–3; the sixth type must be 0), lengths 0–16, every code complete
(Kraft sum exactly 1), decoded bit by bit (MSB first); a 4096-byte window that
starts as spaces, matches of 3–17 bytes at distances 1–4096 that may overlap
themselves and read the initial spaces. KWAJ records no size and has no
checksum, and the stream simply ends: a token counts only once it is
complete, and a read past the last byte ends the file cleanly only when the
unfinished token began fewer than 8 bits before the end (the encoder pads its
last byte with 1-bits, which in 6 of the release's 59 files read as the start
of a token that must produce nothing); otherwise the data ends inside a token
(2). The recipe expands each file once to learn its size, bounded by what is
left of the staging budget (`KwajTooLarge`), and the copy expands no more
than that. A damaged file that still decodes (a flipped literal, a file cut
on a token boundary) is caught only by the manifest (3, before a byte is
written when its size differs), as with SZDD, and not at all under
`--no-verify`.

**InstallShield 2 libraries** (`isz.h`; Marvel Comics Screen Posters'
`IMAGES.1` + `IMAGES.2`, `MODULES.LIB`, `ENGINE.LIB`, `WIN.LIB` and Snoopy's
Screen Savers' `AD_MODS.1` + `AD_MODS.2`: InstallShield 2's compressed "Z"
libraries, PKWARE DCL implode inside; PACKAGES.md §8.9). Written from the
InstallShield format survey's public description and its Python reference
(`research/win/pkg/installshield/`), not from any third-party decoder: it
holds no third-party code. `IszLibrary` takes one library, or every volume
of a split set in any order (ordered by their headers, never their names),
and checks everything the format records: the 255-byte header (signature,
password byte 0, split-set fields, zero padding, DOS dates), the directory
and file tables exactly where the header says and ending every volume,
identical in every volume of a set, entries of their exact sizes with
NUL-terminated names that pass the name rules (code page 437 as UTF-8, no
duplicates), members back to back with no gap or overlap, every size and
sum, a complete set, and exactly one member crossing each boundary between
two consecutive volumes (`IMAGES.1+IMAGES.2!XMEN2099.FIF`). It refuses by
name what neither release has: a password, a stored member, a named or
second directory, a member over three or more volumes, a boundary no member
crosses, and PKWARE's coded-literal mode. The DCL decoder (binary literals;
dictionary bits 4–6, a 1, 2 or 4 KiB window) never copies from before the
first byte written, never produces more than the member's recorded size and
must reach it exactly at the end code, after which only zero bits may
follow; output streams in chunks of at most 64 KiB. A missing volume is
named as the set would name it, unless a file given carries that name: then
the message says which volume that file holds ("the set's volume 1 is
missing (T.1 is volume 2)"). Nothing in the format has a checksum: a damaged
container or stream is a corrupt source (2), and a damaged member that
still decodes to its size is caught only by the manifest (3), as with SZDD
and KWAJ, and not at all under `--no-verify`. The tests carry `ICOMP`
vectors of made-up data and streams written token by token; nothing in
them compresses.

**FAT12/16.** Bytes/sector 512–4096, sectors/cluster a power of two,
reserved sectors ≥ 1, one or two FATs, a root directory, a sector count and
a FAT size; the `55 AA` signature; the image at least as long as its
sectors; FAT12 below 4085 clusters, FAT16 below 65525, FAT32 refused. A
cluster chain that loops, meets a free, bad or out-of-range cluster, or ends
before the file does rejects the source. Deleted, long-name and volume-label
entries and `.`/`..` are skipped; a leading `0x05` is `0xE5`; names are
decoded from code page 437, and so is the volume label (the root's first
label entry, trailing spaces dropped, its case kept: `import.json`'s
`volumeId`). Directory data is read when a directory is
listed, file data only when a file is read. An image from a ZIP is read the
same way, from memory (`FatImage` over the member's bytes).

## Identification

1. **Image md5.** A match with a registry package's known image names the
   package; its fingerprint must then match too. A release on several
   install disks (Star Trek) has a known image of each disk
   (`KnownImage::disk`; both Internet Archive copies of each): the images
   of every disk exactly once (either copy of a disk alike), and nothing
   else, are its known image (`verified: image`). Fewer disks still name
   the package: disk 1 alone is identified and then refused by the recipe
   ("… needs every install disk"); other disks without it are refused as
   what they are ("this image is install disk 2 of 2 of Star Trek: The
   Screen Saver (by its md5); import every disk together (--image …
   --image …, or the ZIP they came in)"). The whole set with other images
   beside it (an unknown image, a second copy of a disk) is identified by
   its fingerprint and verified file by file, and the log says so: "the
   images hold every install disk of Star Trek: The Screen Saver (by md5)
   and another image besides; checking files individually" (a set that
   lacks a disk logs "the source holds install disk 1 of 2 of … (by md5),
   not the whole set" instead). The md5 of a ZIP of install files names a
   package the same way when it is one of its known images (Marvel's two
   ZIPs, Snoopy's, the Looney Tunes', ScreamSavers' and the Disney
   Collection's, Dilbert's flat ZIP); the ZIP is read as the install folder
   (a disk set when it keeps its disks in `DISK<n>` folders), and the
   fingerprint must match. A set's disks may be ZIPs of each disk's files
   (The Far Side's `PNX-FSC1.ZIP`–`PNX-FSC5.ZIP`, Dilbert's
   `DILBERT1.ZIP`–`DILBERT4.ZIP`), each a known image of one disk
   (`KnownImage::disk`), given together and read as one install folder.
2. **Fingerprints** (folders, and images with an unknown md5), every package
   in registry order; exactly one must match:
   * `tree` packages: a FILES dir (`ADE\FILES`, `FILES` or the root) holding
     the first module dir and `ENGINE`, plus the package's marker
     (`ad10`: `AD10TH\ADXPL40.DLL`) and none of its absent dirs (`ad10`: no
     `AD40`). Deluxe: `AD40` + `ENGINE`, as always.
   * `ad3zip` packages: an install dir (`INSTALL`, or the root of a floppy)
     holding `INSTALL.INS`, `SETUP.PKG`, `ENGINE.ZIP`, `MODMISC.ZIP` and
     `AFI.ZIP`; the package is the one whose engine DLL is a member of
     `MODMISC.ZIP` (`ADXPL300.DLL` / `ADXPL40.DLL` / `ADXPL310.DLL` /
     `ADXPL41.DLL` / `ADXPL300.DLL` / `ADXPL100.DLL` for `ad32` / `tt` /
     `simpsons` / `looney` / `screams` / `disney`), whose folder file is a
     member of `AFI.ZIP` (`AD3.AFI` / `PHLEM.AFI` / `SAX.AFI` /
     `LNYTUNES.AFI` / `SCREAMS.AFI` / `DISNEY.AFI`), and whose `marker`, when
     it has one, is a member of `MODMISC.ZIP` too (`ad32`: `AD30RSDB.DLL`).
     Neither archive alone tells the family apart: ScreamSavers ships After
     Dark 3.2's own `ADXPL300.DLL` (1.1.0 took every flat form of it for
     3.2, and `--no-verify` installed it over `packages\ad32`), and every
     `AFI.ZIP` carries other products' folder files. Checked over every known
     source form of the six, disk 1 alone included: each matches exactly one
     package, or none. Central-directory names are not encrypted, so no
     password is needed to identify.
   * `intermission` packages: Presage's installer script `INSTALL.DAT` at the
     source's root (the CD, disk 1, a flat ZIP or a copy of one), a plain file
     of at most 64 KiB (a larger one is simply no match), whose `[data]`
     `shortname` is the package's install name (`SWSE`; keys and sections
     without case, values trimmed), with the package's first archive
     (`SWSE1.ARJ`) beside it. The script is read only for this. Disk 1 alone
     is identified (and then refused: every install disk is needed); disks
     2–5 without it match nothing. A release Delrina's own installer
     installed (`Package::delrina_installer()`: no install name; The Far
     Side, Dilbert) is named by file names alone, nothing read: disk 1's
     tag file `DISK1`, the installer `IMINST2.EXE` and the release's
     `marker` (`PTERY.IMQ`, `DB-CLOCK.IMQ`) side by side at the source's
     root (`importer.cc` `delrina_fingerprint`). Disk 1 alone is identified
     and then refused; any other disk alone, or disk 1 without the installer
     or the marker, matches nothing. Star Wars Screen Entertainment is never
     taken for either, nor either for it; both releases' files in one
     folder match both (ambiguous: `--package` chooses).
   * `ad2kwaj` packages: Microsoft Setup's file list `SETUP.LST` at the
     source's root (disk 1, a copy of it, the disks together, a flat ZIP or
     ISO of their files), a plain file of at most 64 KiB, whose `[Params]`
     `WndTitle` is the package's setup title (`Package::setup_title`,
     "Star Trek®: The Screen Saver", compared in Windows-1252 as the file
     holds it, without ASCII case), with disk 1's tag file (`MISSION.AD_`)
     beside it. The file list is read only for this. An installed
     `C:\AFTERDRK` is no source: it has no `SETUP.LST`, and its `AD.EXE`
     carries the owner's name.
   * `islib` packages: InstallShield 2's package list `SETUP.PKG` at the
     source's root (disk 1, the disks together, a flat folder or ZIP of
     their files) with disk 1's library volume (`IMAGES.1`, `AD_MODS.1`)
     beside it; only then is the list read, once: a plain file of at most
     64 KiB that starts with `4A A3` and parses strictly (`parse_setup_pkg`:
     every group's size adds up, the groups end at the disk table, every
     library points at a group and every group is pointed at), else it is
     no package list and identifies nothing. The package is the one whose
     tag member the list names in its tag library (`Package::tag_library`,
     `tag_member`: `MARVEL.AD` in `modules.lib`, `IS_FLY.AD` in `AD_MODS.z`,
     compared without ASCII case). An AD 3.x install's `SETUP.PKG`, which has
     no such volume beside it, is never read. Disk 1 alone is identified
     (and then refused: every install disk is needed); disk 2 alone matches
     nothing.
3. `--package <id>` restricts step 2 to that package and refuses a source
   that is something else.

No match, several matches, or an md5 that names another package: exit 2,
with a message naming the known releases ("not a known release; known:
…").

## Recipes

**`tree`** copies the package's copy dirs from the FILES dir byte for byte
(`deluxe`: `AD40`, `CLASSIC`, `ENGINE`, `AFI`; `ad10`: `AD10TH`, `ENGINE`,
`AFI` — `GAMES`, `WALLPAPR` and the non-After Dark root folders are left),
then the package's fix-ups: copies under the names the modules open, made
only from a source file that matched the manifest (or came from the known
image). `ad10` has four: `AD10TH\TT_SND.DLL` (from `MUSIC\`), and
`MUSIC\Toasters2k.mid`, `Flying Toasters.mid`, `Baby Toasters.mid`. Their
`import.json` `from` is `alias:<source path in the package>`.

**`ad3zip`** reproduces what the InstallShield scripts did, flattened into
one module dir `M` (the lanes mount it as `C:\AFTERDRK`) and `E` =
`ENGINE`. Every archive's central directory is read; members are
decrypted (traditional PKWARE "ZipCrypto"), raw-inflated through zlib and
checked against their size and CRC-32 (`zip.h`; a mismatch is a corrupt
source, 2, never a verify failure):

| Archive | Goes to |
|---|---|
| any ZIP with an `*.AD` member | every member → `M\` |
| `MODMISC.ZIP` | every member but `EDITFILE.TXT` → `M\` |
| `WIN.ZIP` | `AD_RSRC.DLL` → `M\` |
| `BITMAPS.ZIP`, `TRACES.ZIP`, `SOUNDS.ZIP` | → `M\BITMAPS\`, `M\TRACES\`, `M\SOUNDS\` |
| `MUSICG.ZIP`, else `MUSIC.ZIP` | `*.MID` → `M\MUSIC\`; `*.DLL` (`TT_SND`, `SIMP_SND`, `LT_SOUND`) → `M\` |
| `AFI.ZIP` | the package's folder AFI → `M\FOLDER.AFI` |
| `ENGINE.ZIP` | `AD_SND.DLL`, `ADTASK.DLL`, `ADW30.EXE`, `ADW30.INI`, `ECOLOGIC.DLL` → `E\` |
| `HELP`, `MULTIS`, `WINSYS`, `WAVEMIX`, the unused `MUSIC` | skipped |
| an archive the package lists as never opened (`Package::never_opened`: `disney`'s `BEAUTYOL.ZIP`, the 1993 build of `BEAUTY.AD`) | skipped by name before it is loaded, never read: "skipped BEAUTYOL.ZIP (never opened: not in the recipe of The Disney Collection Screen Saver)" |
| anything else | skipped and logged |

Only the archives and `INSTALL.INS` are read: the Simpsons floppy's
`CEREAL.TXT` and `SERIAL.TXT` (the original owner's notes) are never opened,
copied, hashed or listed, nor are the previous owners' notes in the Looney
Tunes', ScreamSavers' and the Disney Collection's copies. `simpsons`,
`looney` (with `MUSIC.ZIP`), `screams` and `disney` also require all their
module archives, so a split-floppy source must include every disk. The
Looney Tunes install 34 files (12 modules, `ADXPL41`, `LT_SOUND`,
`AD_RSRC`, `FOLDER.AFI`, 13 MIDI; `ENGINE` 5), ScreamSavers 23 (15
modules, After Dark 3.2's `ADXPL300`, which nothing loads but the installer
put there, `AD_RSRC`, `FOLDER.AFI`; `ENGINE` 5) and the Disney Collection 31
(16 modules, `ADXPL100`, `DIS_SND`, `AD_RSRC`, `FOLDER.AFI`, 6 MIDI from
`MUSICG.ZIP`; `ENGINE` 5).

**`intermission`** reproduces what Presage's installer did for Star Wars
Screen Entertainment, flattened into the module dir `M` = `SAVER` (the ne16
lane mounts it as `C:\SAVER`, the modules' current directory), `E` =
`ENGINE` (the guest's `C:\WINDOWS\SYSTEM`) and `WINDOWS` (the files the
installer put in `C:\WINDOWS`; never a module folder). It is baked in, as
`ad3zip` is: registry parameters (the module dir, the archives, the loose
files, the install name), no script interpreter.

1. Every install disk: each of the registry's archives (`SWSE1.ARJ`,
   `SWSE2.ARJ`, `.A01`, `.A02`, `.A03`) must be in the install dir, else 2
   ("the source is missing SWSE2.A01, …; importing … needs every install
   disk").
2. The archives: each chain of volumes that starts at one of the registry's
   `.ARJ` names, followed for as long as a volume's main header says another
   follows (`X.ARJ`, `X.A01`, …) — through the registry's archives only. A
   volume that says the archive goes on to one the registry does not list
   is damaged or foreign: 2 ("SWSE2.A03 says the archive continues on
   SWSE2.A04, which is not one of Star Wars Screen Entertainment's install
   disks"), and the volume it names is never opened, present or not. Any
   other `.ARJ` on the source is skipped (logged); a volume no chain reaches
   is never read.
3. The members, by name:

   | Members | Go to |
   |---|---|
   | `INTERMIS.EXE`, `IMIMXPLY.IMQ` (Intermission, which the host replaces, and its IMX reader) | `E\` |
   | every other `*.IMQ` (the readers of other products' formats, the After Dark reader `IMAD_PLY.IMQ` among them), `AD_SND.DLL` (Intermission's sound support for them, which on a search path would break the After Dark bridge), `IWLIB.DLL`, `*.HLP` | skipped: listed, never decoded (one log line) |
   | everything else: the 14 `*.IMX` modules, the DLLs they load, `SWTEXT.TXT` | `M\` |

4. The loose files, under the installer's names (`packages.h` `LooseFile`,
   INSTALL.DAT lines 4, 10–13 and 37), each only when present (the manifest
   reports a missing one): `STRESS.DL_` → `M\STRESS.DLL`; the General MIDI
   set (`CHECK=24`) `GM_BATTL.MI_`, `GM_CNTNA.MI_`, `GM_EMPIR.MI_`,
   `GM_TITLE.MI_` → `M\BATTLE.MID`, `CANTINA.MID`, `EMPIRE.MID`,
   `SWTHEME.MID` (directly beside the modules, which open them by bare
   name), all SZDD, expanded on the way; `SWSE.INI` → `WINDOWS\SWSE.INI`, as
   is.

Every planned size (an ARJ member's, an SZDD header's) counts towards the
staging budget and is checked against the manifest before a byte is
written. Only `INSTALL.DAT`, the archives and the loose files are ever
opened (I5): the readme, `INSTDETL.DAT`, `INSTALL.EXE`, `SVGA.EXE`,
`SYSINI.DAT`, WinG (`WING*.DL_`, `WINGDIB.DR_`, `WINGPAL.WN_`), `DIB.DR_`,
the VxDs, `IMCPL.CPL`, `SWSESET.EXE` and the other MIDI sets never are.
`import.json`'s `from` names an archive member as `<volume>!<member>`, and
one that spans volumes as `SWSE2.ARJ+SWSE2.A01!JAWAS.IMX`. One manifest
covers this CD (and its Redump BIN and flat ZIP). Other builds are
identified as swse too — the online floppy sets of two earlier US builds, and
the German edition, whose script also says `SWSE` — and a complete set fails
verification (3: the online five-floppy build differs in 7 modules) unless
`--no-verify`, which imports it. The Japanese edition was not examined.

**`intermission` with Delrina's installer** (The Far Side, Dilbert;
`Package::delrina_installer()`) reproduces what Delrina's own Intermission
Installer (`SETUP.EXE` → `IMINST2.EXE`) did: it copied by wildcard into one
folder, `C:\SAVER`, expanding the files COMPRESS had packed under their
installed names. There is no archive and no script: the registry's
`LooseFile` table names every installed file (20 for The Far Side, 23 for
Dilbert), plain or SZDD, each under its own name.

1. Every install disk: each tag file (`DISK1`–`DISK5`, `DISK1`–`DISK4`)
   must be at the source's root, else 2 ("the source is missing DISK2, …;
   importing … needs every install disk"). The tags are only looked for.
2. The loose files: the modules (the `*.ASA` animations and the release's
   IMQ modules) and what they load (`INTRMLIB.DLL`, `ANTSW.DLL`,
   `DIBDLL.DLL`, `MEMMIDI.DLL`, Dilbert's `IM4_EXP.DLL`) → `M\` = `SAVER`;
   the ASA reader `IMASAPLY.IMQ` and `INTERMIS.EXE` (kept for reference)
   → `E\` = `ENGINE`. No `WINDOWS` folder.

Only the table's files are ever opened (I5): the installer's programs,
`AD_SND.DLL`, the other readers, `IWLIB.DLL`, `NETPASS.EXE`,
`SSINTERM.SCR`, the VxD, the control panel, the sound drivers, the texts
(Dilbert's `PACKING.LST` among them), `ICONDLL.DLL`, `ANTSW2.DLL`,
`MAPI.DLL`, `INTERMIS.LIB` and whatever else a copy holds (the bulletin
boards' notes) never are. `import.json`'s `from` is the file's name on the
disks (`AERIAL.ASA`).

**`ad2kwaj`** reproduces what Microsoft Setup (`ST_NSTLL.INF` and the MS-Test
script `AD_NSTLL.MST`) did for Star Trek: The Screen Saver, the After Dark 2.0b
release, flattened into the module dir `M` = `AFTERDRK` (the installer's
folder, which the 16-bit lane mounts as `C:\AFTERDRK`) and `E` = `ENGINE`.
It is baked in: registry parameters (the module dir, the disks' tag files,
the loose files, the setup title), no INF or MST interpreter. There is no
`WINDOWS` folder: the lane's profile seeds are the modules' settings.

1. Every install disk: each tag file of the INF's `[Source Media
   Descriptions]` (`MISSION.AD_` on disk 1, `ST_SND.DL_` on disk 2) must be
   at the source's root, else 2 ("the source is missing ST_SND.DL_; importing
   Star Trek: The Screen Saver needs every install disk").
2. The loose files (`packages.h` `LooseFile`, all `Codec::kwaj`),
   KWAJ-expanded under their installed names, each only when present (the
   manifest reports a missing one); nothing is edited:

   | Files | Go to |
   |---|---|
   | the 16 modules, `BRAINCEL.AD_` … `TRIBBLE.AD_` | `M\*.AD` |
   | `AD_MOD.DL_`, `AD_RSRC.DL_` (the modules' framework, which the installer put in `C:\WINDOWS`: they import it, I2), `AD_MME.DR_` (the multimedia sound driver AD_SND loads from `[Sound] SoundDriver`) | `M\AD_MOD.DLL`, `M\AD_RSRC.DLL`, `M\AD_MME.DRV` |
   | `ST_RESDB.DL_`, `ST_MASKS.DL_`, `ST_VGA.DL_`, `ST_SVGA.DL_`, `ST_SND.DL_` (the art and sound databases AD_MOD opens from `<Path>ST_RES\`) | `M\ST_RES\*.DLL` |
   | `JIM.WA_` (Sounder refuses to start without a `.WAV`) | `M\SOUNDS\JIM.WAV` |
   | `AD_SND.DL_` (AD_SND 1.0, which the native bridge loads) | `E\AD_SND.DLL` |
   | `AD.EX_` (After Dark 2.0's host, which the host replaces: kept for reference, as `INTERMIS.EXE` is; nothing loads it) | `E\AD.EXE` |

27 files, 5,187,460 bytes. KWAJ records no size: each file is expanded
once while planning to learn it (bounded by what is left of the staging
budget), and that size counts towards the budget and is checked against the
manifest before a byte is written; the copy expands no further. The files
keep their FAT timestamps. Only `SETUP.LST` (to identify) and the table's
27 files are ever opened (I5). The other 33 files of the INF's 61 lines
never are, nor the INF itself (34 of the disks' 62 files): the disk's
`AD_PREFS.INI` (its `SoundDriver=AD_MPT.DRV` would win
over the lane's profile seed and hang the emulator), the PC-speaker path
(`AD_MPT.DRV`, `SPALETTE.DLL`, `AD_LIB.DLL`), `AD_SB.DRV` (it refuses
Windows 3.1 and later), the network support (`AD_AILAN.DLL`, `AD_NVLNW.DLL`,
`AD_NET.EXE`, `NWCORE.DLL`, `NWCONN.DLL`, `NWMISC.DLL`), `AD.386`,
`AD_WRAP.COM`, `AFTERDRK.NSS`, `ADINIT.EXE`, `AD.HLP`, `AD_MESG.ADS` (the
data of a Messages module this release does not ship), `AD_NSTLL.INI` and
Microsoft Setup's own files (`SETUP.EXE`, `_MSTEST.EXE`, `AD_NSTLL.MST`,
`AD_NSTLL.DLL`, the `MS*STF.DLL`s, `VER.DLL`, the `.INC` files,
`SPLASH1.BMP`, `BMPRSRC.DLL`). `import.json`'s `from` is the compressed file
(`PLANETS.AD_`).

**`islib`** reproduces what InstallShield 2.00 did for Marvel Comics Screen
Posters (`INSTALL.INS`) and Snoopy's Screen Savers (`SETUP.INS`): each
member of their compressed libraries placed as the script placed it,
flattened into the module dir `M` = `AFTERDRK` and `E` = `ENGINE`. It is
baked in: registry parameters (the module dir, the library volumes, the tag
and the placement table `Package::library_members`, one `LibraryMember` per
installed file: 64 rows for `marvel`, 8 for `snoopy`), no script
interpreter.

1. Every install disk: each library volume the registry lists must be at
   the source's root, else 2 ("the source is missing IMAGES.2, MODULES.LIB,
   ENGINE.LIB, WIN.LIB; importing Marvel Comics Screen Posters needs every
   install disk").
2. Each library the table names is read once, whole, with `isz.h`: a file,
   or a split set found from its first volume's header, its other volumes
   named from it (`IMAGES.1` → `IMAGES.2`) and taken only from the
   registry's volumes. Refused (2): a volume that says the set continues on
   one the registry does not list ("IMAGES.1 says the library continues on
   IMAGES.3, which is not one of the install disks of Marvel Comics Screen
   Posters (a damaged or foreign volume?)"; that file is never opened), a
   first volume that holds another volume ("IMAGES.1 is volume 2 of its
   set, not volume 1 (a mislabelled or foreign volume?)"; nothing else of
   the set is opened), and any damaged library or stream.
3. The table's members go where it says: `MARVEL.AD` and `DECO.DLL` →
   `M\`; the 36 posters, 23 tables and the image catalog `MRVLIMAG.ADC` of
   `IMAGES.1`+`IMAGES.2` → `M\MRVLIMAG\`; `ENGINE.LIB!AD.EXE` (After Dark
   2.0d's host, kept for reference) and `WIN.LIB!AD_SND.DLL` (AD_SND 1.0)
   → `E\`; Snoopy's eight modules → `M\`. A member no row names is listed
   in one log line ("skipped ENGINE.LIB!ADINIT.EXE, … (not in the recipe
   of …)") and never decoded.
4. Libraries without the tag member `SETUP.PKG` promised are not the disks
   of one release (2: "SETUP.PKG lists MARVEL.AD in modules.lib, but
   MODULES.LIB holds no such member (not the disks of one release?)");
   another missing member is logged ("the source's IMAGES.1 has no
   COVER.FTT") and the manifest reports it (`partial`).

`marvel` installs 64 files, 2,002,020 bytes (`AFTERDRK` 62, `ENGINE` 2);
`snoopy` 8, 4,044,927 bytes, and no `ENGINE` (its modules were made for an
After Dark already installed; the ne16 lane supplies their AD_SND,
PACKAGES.md §7.4). `from` is the member's volumes and name
(`IMAGES.1+IMAGES.2!XMEN2099.FIF`, `MODULES.LIB!MARVEL.AD`), the copy's time
the member's DOS time. Every recorded size counts towards the staging
budget and is checked against the manifest before a byte is written; a
changed literal that still decodes is caught by the manifest (3). Only
`SETUP.PKG` (to identify) and the registry's volumes are ever opened (I5):
Marvel's `INSTALL.INS`, `SETUP.EXE`, `SETUP.BIN`, `~INS0762.LIB`,
`CHANGES.TXT` and `WINSYS.LIB`, and the rest of `ENGINE.LIB` and `WIN.LIB`
(listed, never decoded: the PC-speaker, Sound Blaster and multimedia
drivers, `AD_LIB.DLL`, `ADINIT.EXE`, the manual and readme texts, `AD.HLP`,
`AD_WRAP.COM`, `SPALETTE.DLL` and the disk's `AD_PREFS.INI`); Snoopy's
`SETUP.EXE`, `SETUP.INS`, `AD_MODS.LIS` and `AD_MODS.BMP` (a cover source
only), the disk copier's leftovers (`AD_Changes.txt`, `CMOS.RAM`,
`DREAM.ON`, `TXTSCR.DAT`); and both copies' previous owners' notes, which
nothing reads.

**The archive password** is never stored: it is derived from `INSTALL.INS`
at import time (§8.4). Candidates are the script's strings of 4–32 printable
bytes — its own length-prefixed strings (a 16-bit length, then the bytes:
the real scripts follow the password with an opcode byte that is printable,
so a bare printable run would be one byte too long) and plain printable
runs — the first one after "Cannot initialize for unzip!" first, then the
rest in file order. The first candidate that opens the smallest encrypted
member (check byte, then full decrypt, inflate and CRC-32) and the smallest
encrypted member of another archive is the password. It is kept in memory
only: never logged, never in `import.json`.

**Required files and invariants.** A package's `required` files (§2) must be
in the plan (2 otherwise). After staging, every package but Deluxe must
satisfy PACKAGES.md §4.2 (2 otherwise): I1 no `AD_SND`/`OLDMOD16`/`OLDMOD32`/
`ADTASK`/`ADW30.EXE` beside the modules; I2 every non-system DLL a module
imports (but `AD_SND`) beside it; I3 `ENGINE\AD_SND.DLL` plus either
`ENGINE\OLDMOD16.DLL` + `AFTERDAR.SCR` or `ENGINE\ADTASK.DLL`; I4 every
sound database (a `*_SND.DLL` other than `AD_SND.DLL`, or a `*_SOUND.DLL`:
the Looney Tunes' `LT_SOUND.DLL`) in a module folder, and (`ad3zip`) every
MIDI in `M\MUSIC\`. The Looney Tunes require `LNYTUNES\ADXPL41.DLL` and
`LT_SOUND.DLL`, ScreamSavers only `ENGINE\AD_SND.DLL` and `ADTASK.DLL`, the
Disney Collection `DISNEY\ADXPL100.DLL` and `DIS_SND.DLL`, each with its
`ENGINE\AD_SND.DLL` and `ADTASK.DLL`. An `intermission` package has its own: I1 also no
`INTERMIS.EXE` or `*.IMQ` beside the modules (they live in `ENGINE`); I2 also
every non-system DLL that the NE DLLs beside the modules import is beside them
too (`INTRMLIB.DLL` → `ANTSW.DLL`; `TOOLHELP`, `LZEXPAND`, `VER` and `WING`
count as system here); I3 instead: `ENGINE\IMIMXPLY.IMQ` and the installer's
`WINDOWS` files (`SWSE.INI`) exist, and `ENGINE` holds none of
`OLDMOD16.DLL`, `ADTASK.DLL`, `AD_SND.DLL` (so the 16-bit lane can never take
it for an After Dark package); I4 every MIDI directly in a module folder.
Its `required` files are `SAVER\{INTRMLIB,ANTSW,SWSE,READJPG,STRESS,SWSFX,MEMMIDI}.DLL`,
`ENGINE\IMIMXPLY.IMQ` and `WINDOWS\SWSE.INI`. With Delrina's installer
(The Far Side, Dilbert), I1 allows the release's own IMQ modules beside
the modules (the `*.IMQ` the registry places there), never a file named as
Intermission's readers are (`IM???PLY.IMQ`), and I3 wants
`ENGINE\IMASAPLY.IMQ` when the module folder holds ASA animations instead
of `ENGINE\IMIMXPLY.IMQ`; their `required` files are
`SAVER\{INTRMLIB,ANTSW,DIBDLL,MEMMIDI}.DLL` (Dilbert's also `IM4_EXP.DLL`)
and `ENGINE\IMASAPLY.IMQ`. An `ad2kwaj` package has its
own too: I1 also no `AD.EXE` beside the modules (it is kept in `ENGINE`); I2
also every non-system DLL that the NE DLLs and drivers (`*.DRV`) beside the
modules import is beside them (`AD_MOD.DLL` → `AD_RSRC`; `AD_SND` is
`ENGINE`'s, and the intermission recipe's system list applies); I3 instead:
`ENGINE\AD_SND.DLL` exists, `ENGINE` holds none of `OLDMOD16.DLL`,
`ADTASK.DLL`, `AFTERDAR.SCR` (After Dark 3.x and 4.x's), and there is no
`WINDOWS` folder at all; I4 the sound database (`ST_SND.DLL`) in
`AFTERDRK\ST_RES\`, where AD_MOD opens it. Its `required` files are
`AFTERDRK\{AD_MOD,AD_RSRC}.DLL`,
`AFTERDRK\ST_RES\{ST_RESDB,ST_MASKS,ST_VGA,ST_SVGA,ST_SND}.DLL` and
`ENGINE\AD_SND.DLL`. An `islib` package has its own as well: I1 also no
`AD.EXE` beside the modules; I2 also every non-system DLL that the NE DLLs
and drivers beside the modules import is beside them (Marvel's
`DECO.DLL`; `AD_SND` is `ENGINE`'s); I3 instead: `ENGINE\AD_SND.DLL` when
the table places one (Marvel; Snoopy ships none), `ENGINE` holds none of
`OLDMOD16.DLL`, `ADTASK.DLL`, `AFTERDAR.SCR`, and there is no `WINDOWS`
folder; I4 as for `ad3zip`. Marvel's `required` files are
`AFTERDRK\DECO.DLL`, `AFTERDRK\MRVLIMAG\MRVLIMAG.ADC` and
`ENGINE\AD_SND.DLL`; Snoopy's are none (its tag member, which the recipe
requires, is a module).

## Destination and atomicity

`<root>` is `--dest`, else `%AD_ASSETS_DIR%`, else `<data folder>\assets`.
Everything goes to `win_assets_dir(<root>)`: `<root>\win` when it holds
`FILES`, `packages` or `catalog-win.json`; else `<root>` itself when that
holds one of them; else `<root>\win` — the rule `adhostwin` applies to
`AD_ASSETS_DIR`.

**The data folder** is Long After Dark's per-user folder,
`%LOCALAPPDATA%\LongAfterDark`, which the host and `LongAfterDark.scr` use
too; the default assets root and the default downloads folder
(`<data folder>\downloads`, with cover pictures in its `covers\`) are in it.
`importer.h` `data_folder()` is `<base>\LongAfterDark`, on the same base as
the host and the saver (the shared helper
`host/core/include/adw/core/data_root.h`, whose include directory
`adw_import` adds privately): `AD_LOCALAPPDATA` when set and not blank, else
`LOCALAPPDATA`, else `SHGetKnownFolderPath(FOLDERID_LocalAppData)`. Working
it out reads the environment only, never the disk, and creates nothing;
nothing is created under it until an import or a download writes there. A
run with explicit locations never uses it: `--dest` (or `AD_ASSETS_DIR`),
and `--download-dir` for a download. Cover commands and imports take the
downloads folder only when a cover download actually starts. `--help` names
the folder.

**One package, one directory.** One operation per win dir at a time
(`import.lock`, delete-on-close); holding it makes the recovery below safe.
*Deluxe* is staged in `FILES.importing-<pid>`, re-read and re-hashed, and
swapped in with two renames, `import.json` and the catalog after it — the
flow it always had. *Every other package* is staged in
`packages\<id>.importing-<pid>` (files re-read and re-hashed, the manifest,
`required` and the invariants checked, its `import.json` written into the
stage), the merged catalog is rendered over the stage plus every other
installed package, then `packages\<id>` → `packages\<id>.old-<pid>`, stage →
`packages\<id>`, catalog tmp → `catalog-win.json`, old tree deleted. An
import of one package writes only its own directory, `catalog-win.json*`
and `import.lock`; it never modifies `FILES`, `import.json` or another
package. Cancel is honoured up to the first rename and ignored after it (a
finished import never reports 5).

**Recovery**, at the start of every operation under the lock: Deluxe's
(unchanged: no `FILES` but a `FILES.old-<pid>` is put back; a finished swap
whose `import.json.tmp`/catalog tmp renames did not happen is completed);
then every `packages\<id>.old-<pid>` is put back when `packages\<id>` is
missing, else deleted; every `*.importing-*` and `*.removing-*` is deleted;
and the catalog is rewritten when anything was recovered or a package
operation's catalog tmp was left behind. Folders in `packages\` that are not
registry ids are ignored (and logged).

*Installed* means: Deluxe when `<win>\FILES` is a directory; another
package when `<win>\packages\<id>\import.json` exists. `--catalog-only`
needs one installed package, not `FILES`.

**Exit codes** (`adw::import::Status`): 0 ok · 1 error (usage, local I/O,
another operation running, `--remove` of something not installed) · 2
source invalid · 3 verify failed · 4 network · 5 cancelled.

## Verification

Each package has a manifest (path, size, md5 of every installed file,
fix-ups included — never After Dark bytes): `known_files.inc` (Deluxe, 175
files) and `known_files_<id>.inc` (`ad10` 147, `ad32` 89, `tt` 26,
`simpsons` 30, `swse` 29, `startrek` 27, `marvel` 64, `snoopy` 8, `looney`
34, `screams` 23, `disney` 31, `farside` 20, `dilbert` 23). `"verified"` is `image` (the image md5 is one of
the package's known images — a disc or floppy image, or a known ZIP of the
install files —, or the images are a known disk set), `files` (every file of the manifest is there and matched it, and
nothing else was installed), `partial` (some installed files are not in the
manifest, or some of the manifest's are missing: `missingKnown` lists them),
or `none` (`--no-verify`, or no manifest). A file that differs
from the manifest fails the import with 3 unless `--no-verify`. Regenerate a
manifest with `gen_known_files.py <import.json of a verified image import>`:
it accepts Deluxe's version-1 record and every other package's version-2
record, only when the image md5 is one of that package's known images
(`swse`: the ISO or the Redump BIN, which give the same files) — for a
release on several install disks (`startrek`), only when the record's
`parts` are every one of its known disks exactly once (either copy of each)
and nothing else, and it has no single image md5; the manifest's header
then names the disks' md5s. For a release known by the ZIP of its install
files (`marvel`: either of its two ZIPs, which give the same files;
`snoopy`, `screams`, `disney`; `looney`: its ZIP or its `LOONEY_T` CD) it
takes the import of that ZIP, and the header says so. A release known by a
ZIP of each disk's files (`farside`) is a disk set of ZIPs, whose header
says "ZIPs"; one known both ways (`dilbert`: its flat ZIP, or its four
ZIPs, which give the same files) takes either. The script writes next to
itself: run it on a scratch copy to compare.

## import.json

Deluxe keeps `<win>\import.json`, version 1, unchanged:

```json
{ "version": 1, "tool": "adimport 1.0", "importedUtc": "2026-09-25T23:10:00Z",
  "source": { "kind": "download", "path": "C:\\…\\downloads\\After Dark 4.0 Deluxe….iso",
              "url": "https://archive.org/download/…", "finalUrl": "https://…archive.org/…",
              "isoSize": 400234496, "isoMd5": "d875a60338b73f44b7befa06bdd33aeb",
              "isoMd5Known": true, "joliet": false, "volumeId": "AD_DELUXE" },
  "verified": "image", "fileCount": 175, "totalBytes": 34351595, "missingKnown": [],
  "files": [ { "path": "FILES/AD40/3DMINOR.MID", "size": 17373, "md5": "…", "known": "match" } ] }
```

Every other package writes version 2 inside its root (PACKAGES.md §5.3):

```json
{ "version": 2, "tool": "adimport 1.3", "importedUtc": "…",
  "package": {"id": "ad32", "title": "After Dark 3.2", "recipe": "ad3zip", "root": "packages/ad32"},
  "source": { "kind": "iso", "format": "iso9660", "path": "D:\\…\\afterdark3.2.ISO",
              "imageSize": 61693952, "imageMd5": "8b8be6977375fbf4d54146b9d505aa1c", "imageMd5Known": true,
              "volumeId": "ADW320_C", "parts": [] },
  "verified": "image", "fileCount": 89, "totalBytes": 6037078, "missingKnown": [],
  "files": [ {"path": "packages/ad32/AD32/GUTS.AD", "size": 16688, "md5": "…", "known": "match",
              "from": "INSTALL/GUTS.ZIP!GUTS.AD"} ] }
```

`package.recipe` is `tree`, `ad3zip`, `intermission`, `ad2kwaj` or `islib`. `kind` is `iso`,
`floppy`, `zip`, `folder` or `download`; `format` is
`iso9660`, `iso9660+joliet`, `fat12`, `fat16`, `zip` or `folder`; the image
fields appear for image sources, downloads included (`imageSize`/`imageMd5`
for a single image: for the Simpsons download, the ZIP's); `parts` lists
every image of a multi-image source, an image from a ZIP as
`<zip path>!<member>` (`…\afterdark-20b_startrek.zip!afterdark-20b_startrek_disk2.img`),
and `imageMd5Known` is true for a known disk set too. A download adds `url` (the copy
fetched; for a floppy set, its first image's), `finalUrl` (where its redirects led; absent when an earlier
download was reused) and `md5Checked` (the file matched its published md5,
or `--md5`). Deluxe's version-1 record keeps its `url`/`finalUrl`. `from` is the source path, `zip!member`
for an archive member (`SWSE1.ARJ!ANTSW.DLL`; `SWSE2.ARJ+SWSE2.A01!JAWAS.IMX`
for an ARJ member that spans volumes; `MODULES.LIB!MARVEL.AD` and
`IMAGES.1+IMAGES.2!XMEN2099.FIF` for InstallShield library members), a
path in the union for a disk set (without its `DISK<n>` folder),
`alias:<path>` for a fix-up. `files` is sorted by
path; `known` is `match`, `mismatch` or `unknown`.

Both records are UTF-8 JSON whatever a source holds. Every name reaches
them as UTF-8 (a folder's as Windows lists them; FAT and ARJ names, FAT
volume labels and ZIP member names that are not UTF-8 decoded from code
page 437; ISO-9660 names and the volume id as Latin-1, Joliet names as
UCS-2), and the one string escaper both are written with (`minijson.h`
`json_escape`: `"`, `\` and the C0 controls escaped) writes any byte that
still starts no well-formed UTF-8 sequence as U+FFFD, so
`gen_known_files.py`, which reads a record as strict UTF-8, can always read
it.

## Catalog

`<win>\catalog-win.json` is what the front-ends read (DESIGN.md §6a,
PACKAGES.md §6). It is generated from the module binaries without executing
anything (`catalog.h`, a C++ port of the prototype
`research/win/make_catalog.py`, using the `adw::loader` PE/NE readers),
following ABI.md §2.10, over every installed package in registry order:

| | AD4 lane (`pe32`) | Classic lane (`ne16`) | Intermission IMX (`ne16`) |
|---|---|---|---|
| Lane | the file header says PE32 | the file header says NE, and it exports `MODULE` | NE, and exports `SAVERINIT` and `SAVERDRAW` but neither `MODULE` nor `SETCURRSAVER`, in a file not named `IMXX_*` (IMIMXPLY.IMQ's own rules) |
| Files, in order | Deluxe: `AD40\*.AD` sorted, `ENGINE\STARRYNI.AD`, `CLASSIC\*.AD` sorted; every other package: its module dirs in registry order, then `ENGINE`, each dir's `*.AD` and `*.IMX` sorted together (`WINDOWS` never) | same | same (`packages\swse\SAVER\*.IMX`) |
| `id` | Deluxe `ad40.<base>`; else `<package>.<base>` | Deluxe `classic.<base>`; else `<package>.<base>` | `<package>.<base>` |
| name | `VERSIONINFO` `FileDescription` | resource `2000/20` | none in the file: the file stem, then the registry's name overrides (the names `SAVERINIT` returns) |
| `about` | `2000/40`, RTF reduced to plain text | `2000/30` (After Dark 2.0's: see below); `credits` from `2000/10` | `""`, no `credits` |
| `controls` | `1000/1..4` (slot = name − 1) | same | one button, `{index 0, "Configure...", button}`, when `SAVERDLGPROC` is exported (the module's own dialog) |
| `entry` | `_Module@4` when exported (STARRYNI), else `Module` | `MODULE` | `SAVERDRAW` |
| `abi` | absent (After Dark) | absent (After Dark) | `"intermission"`, the entry's last field |
| `screen` | absent | `"640x480"` on every `startrek`, `screams` and `marvel` entry, its last field; else absent | absent (the ABI gives an IMX module its screen) |

**Intermission's other two forms** (The Far Side's and Dilbert's `SAVER`,
the module folders of a release Delrina's installer installed; no other
folder lists `*.ASA` or `*.IMQ`, and `ENGINE` never does): an *ASA
animation*, a `*.ASA` that starts `AniN` or `AniM` (data, played by
Intermission's ASA reader from `ENGINE`), and an *IMQ module*, an NE
`*.IMQ` that exports `SAVERMAIN` but neither `MODULE` nor `SAVERINIT` and
`SAVERDRAW` (its own reader), unless it is named as Intermission's readers
are, `IM???PLY.IMQ` (`catalog.h` `is_intermission_reader`: left out and
logged). Both are lane `ne16`, `abi` `"intermission"`, `entry`
`"SAVERMAIN"`, `about` `""`, with one button, `{index 0, "Configure...",
button}`: always for an animation (its reader's dialog), for an IMQ module
when it exports `SAVERDLGPROC`. An IMQ module's `needs` are its imports; an
animation has none. No file holds a name: the registry's overrides give
them (below). No `screen`: the ABI gives them 640×480.

An NE file is told apart exactly as the ne16 lane's `detect_kind` tells it
(`host/ne16/package.cc`), so the catalog lists it as the lane will run it:
by its exports' names (without case; a name needs no entry-table entry, as
in the lane), never by the extension. `MODULE` wins when both kinds' exports
are there. An NE the lane would refuse — an `IMXX_*` file, one exporting
`SETCURRSAVER`, `SAVERINIT` or `SAVERDRAW` alone, a reader exporting
`SAVERMAIN`, one with none of these — is left out, logged with the lane's
reason (`catalog: skipped packages/swse/SAVER/IMXX_EXT.IMX: an Intermission
module named IMXX_*, which the IMX reader refuses`). All 230 NE modules of
the eleven After Dark releases export `MODULE`; Star Trek's 16, After Dark
2.0b's, are Classic-lane modules like the others, and so are Snoopy's eight,
which export it from the NE non-resident names table (searched as the lane
searches it).

A missing name falls back to `STRINGLIST 128[0]`, then the file name; it is
trimmed at both ends (After Dark 2.0 began 15 of Star Trek's 16 names with
a space), then the package's name overrides apply (`ad10`
`TOAST2K.AD` → "Toasters 2k (early build)"; every `swse` module, `SAVER/VADER.IMX`
→ "Darth Vader"; `startrek` `AFTERDRK/PLANETS.AD`, whose resource says
" PlanetaryAtlas", → "Planetary Atlas", as its About heading and Berkeley's
later `PREVIOUS.INF` name it; `disney`'s five names that lost their space
to the 16-byte name resource, `DISNEY/DALM.AD` → "101 Dalmatians",
`DSCLOCKS.AD` → "Disney Clocks", `FALLING.AD` → "Falling Flower",
`FIREWRK.AD` → "Magic Kingdom" and `MERMAID.AD` → "Little Mermaid", as each
module's own description spells it; every `farside` module, from its ASA
header's title or its own strings without the "FS-" prefix,
`SAVER/HELL.ASA` → "Hell"; every `dilbert` module, as its installer's
module list `PACKING.LST` names it, `SAVER/DIL-WHAK.IMQ` → "Budget Woes"):
that is `moduleName`.
`displayName` is unique within a lane, case-insensitively: the first module
with a name keeps it, a later one becomes `name (<short title>)`, and if
that is taken too `name (<short title>, <FILE>)` — so today's front-end,
which lists by lane, needs no change. (The Looney Tunes' Messages is
"Messages (Looney Tunes)" beside Deluxe's and 3.2's, "Messages" on its
own.) PE resources in several languages
(STARRYNI has five) are read as an English Windows loads them: 0x409, then
neutral, then the lowest id. Text is Windows-1252, written as UTF-8. Each
control carries `index`, `name`, `kind`
(`stringslider`/`numslider`/`popup`/`checkbox`/`button`) and `type`, plus per
kind: string sliders `items` + `values` (the value sent for each stop; the
host's prepended 0 stop and repeated, bold last label included), `default`
(a value) and `defaultStop` (+ `boldStop`); numeric sliders `min`/`max`,
`default` clamped into them and `rawDefault` as stored, `unit` + `unitPos`
when the record names a unit; popups `items` + clamped `default`; checkboxes
`default` 0/1. Modules also list `needs` (non-system DLLs they import) and
`system`, then `package`, `packageTitle`, `moduleName`, `md5` (of the file),
when an earlier entry has the same bytes, `sameAs` (its id), and for an
Intermission module `abi` (`"intermission"`; absent means the After Dark
module ABI, so every After Dark entry is laid out exactly as before), and
last, for a package shown at a fixed screen, `screen` (`Package::screen`:
`"640x480"` on all 16 `startrek` entries, so the release looks as it did at
640×480: some of its modules — The Mission, Final Exam and Sickbay — compose
a fixed 640×480 scene, and the others (Ship Panels and Scotty's Files among
them) lay out for whatever screen they get; on all 15 `screams` entries,
four of whose modules paint a 640×480 scene and one composes one 640 wide;
and on `marvel.marvel`, whose posters are fixed 640×480 pictures; the
settings window gives such a module that screen whatever the Resolution
setting, as it gives an IMX module its own by ABI; absent everywhere else).

**After Dark 2.0's About texts** (`Package::About::ad20`, `startrek` only;
`catalog.h` `ad20_about`): the last line, the registrant stand-in "Berkeley
Systems Authorized User." (After Dark 2.0 wrote the owner's name in its
place), is dropped with the blank lines before it, and a line break with a
space before it and a lower-case letter after it (a sentence wrapped by
hand: six in the release) is joined into that space. The rules are the
package's: the join would change four entries of other releases
(`classic.dominoes`, `classic.om`, `ad32.mmas`, `simpsons.lisa`), which are
listed as written.

The top-level `packages` list (between `generator` and `modules`) gives each
installed package's `id`, `title`, `shortTitle`, `root`, `verified`,
`importedUtc`, module count and `cover` (COVERS.md §2.7; paths relative to
`<win>`):

```json
"cover": { "origin": "download", "tile": "covers/simpsons/tile.png", "tileMd5": "…",
           "image": "covers/simpsons/original.png", "width": 600, "height": 776, "art": "box",
           "label": "Box front", "credit": "Wikisimpsons", "original": "download" }
```

`origin` is `user`, `download`, `disc` or `generated` (then the object holds
nothing else); `original` is the origin of the original, under a user picture
too. Every catalog write checks the covers: a missing, damaged or stale tile
whose picture is sound is rendered again, and a damaged `cover.json` lists as
generated (logged) until `--refresh-covers` repairs it. The generator is
`adimport 1.3` (1.2 was the covers; 1.3 adds `abi`, `screen` and the
`intermission` and `ad2kwaj` recipes; the catalog `version` stays 1, and
every entry of the releases before them is as it was);
the layout matches Python's
`json.dump(indent=1, ensure_ascii=False)`, so it diffs cleanly against the
prototype's.

A module that cannot be read, or that its lane would refuse, is left out
and logged, never a reason to refuse the import; two files with one id keep
the first. The catalog is
rendered from the stage and swapped in with the files (a failed import
leaves the old one); `--catalog-only` holds `import.lock` while it scans and
replaces the file atomically.

Over the real corpus: Deluxe 84 (23 `pe32` + 61 `ne16`; semantically
identical to the prototype's apart from the new fields and one correction —
the prototype never parses exports, so it lists STARRYNI's entry as
`Module`; the module exports `_Module@4`), `ad10` 46 (17 + 29), `ad32` 44,
`tt` 13, `simpsons` 15 (all `ne16`), `swse` 14 (all `ne16`, Intermission),
`startrek` 16 (all `ne16`, After Dark 2.0b, each with `screen`), `marvel` 1
(with `screen`, two buttons), `snoopy` 8 (27 controls, no buttons, `about`
empty: their About texts are blank), `looney` 12 (one button), `screams` 15
(each with `screen`), `disney` 16 (no buttons), `farside` 14 and `dilbert`
16 (all `ne16`, Intermission: 12 and 13 ASA animations, 2 and 3 IMQ
modules, one button each): 314 with all fourteen installed, 232 over the
first seven, 73 of them `sameAs` an earlier entry (swse, startrek and the
seven after them add none); the `packages` list, oldest first, reads
`startrek` (1992-11), `marvel`, `farside`, `simpsons`, `swse`, `snoopy`,
`dilbert`, `looney`, `screams`, `ad32`, `tt`, `disney`, `deluxe`, `ad10`.

## Covers

Each imported release has a box cover (`docs/COVERS.md` §2; the settings
dialog's release strip shows them). The tile shows the first of:

1. **your own picture**, set with `--set-cover` (or "Change cover…" in the
   GUI);
2. **the original**: the best of the release's *cover sources* captured so
   far (`packages.h` `CoverSource`, tried in the release's own order);
3. **a generated cover**, which the front-ends draw themselves.

| id | Sources, in the order tried | original.png |
|---|---|---|
| `deluxe` | the box front from Berkeley Systems' 1997 product page, through the Wayback Machine (`box.deluxe.gif`), then the setup wizard's art on the disc (`ADE\PAGE1.BMP`), then the disc label (`archive.org/download/after-dark-4-deluxe/disc.jpg`) | 162×195 / 118×226 / 1488×1452 |
| `ad10` | the disc label (`ad10th/01_ad10_cd.jpg`), then `ADE\PAGE1.BMP` | 800×794 / 118×226 |
| `ad32` | the installer splash on the disc (`INSTALL\SETUP.BMP`, cropped to 387×183 above its warning), then two scans of the disc label | 387×183 |
| `tt` | the box front from Berkeley Systems' 1997 product page (`box.twistedL.jpg`), then the box front of Sierra's later edition from The Sierra Chest (`01_front.JPG`), both through the Wayback Machine, then the installer splash (`INSTALL\SETUP.BMP`, cropped to 387×204), then the `TTW320CD` item's TIFF scan of the disc | 127×162 / 498×599 / 387×204 / 2014×2048 |
| `simpsons` | the box front from Wikisimpsons, then the Wayback Machine's copy of the full-box scan it was cut from (cropped to 600×776), then `SETUP.EXE`'s bitmap 7500 (cropped to 387×172) | 600×776 / 387×172 |
| `swse` | the box front from Presage's 1997 product page (`box-starwars.JPEG`), then Wookieepedia's photo of the same box (the Wayback Machine's byte-exact capture of the stored original: Fandom's live URL rewrites what it serves), both through the Wayback Machine, then two archive.org scans of the disc label (`swse1/1.jpg`, and the exact-ISO item's, cropped to 1424×1424); nothing on the disc (every picture is inside its ARJ archives), so an offline import shows the generated cover | 150×200 / 713×847 / 1416×1416 / 1424×1424 |
| `startrek` | the Windows box front from the Internet Archive's box scans (`afterdark-20b_startrek_box/Aaa_itemimage.jpg`; it fills the tile), then the same front at 600 dpi, then the scan of disk 1's label in the disk images' own item (`afterdark-20b_startrek_disk1.jpg`, art `panel`: drawn as a picture, never cut to a disc); nothing on the disks (every file is KWAJ-compressed), so an offline import shows the generated cover | 1180×1525 / 1585×2048 / 1090×1145 |
| `marvel` | the Internet Archive's photo of the Windows box front (`afterdarkmarvelscreenposters/box.jpg`, the flat ZIP's item; cropped to the box at 28,64, 1132×1390, the camera's date stamp inside it kept), then the 1993 magazine advertisement showing the same box (`marval-computer/MarvalComputer.jpg`, art `panel`, "Advertisement"); nothing on the disks (every picture is inside the libraries), so an offline import shows the generated cover | 1132×1390 / 1307×2048 |
| `snoopy` | the picture its installer shows beside the readme on disk 1 (`AD_MODS.BMP`, Image Smith's logo, art `panel`, "Setup art"); no box, label or manual scan of the Windows release was found online | 79×175 |
| `looney` | the box front from Berkeley Systems' 1997 product page (`box.looneytunesL.jpg`, through the Wayback Machine), then the installer splash (`SETUP.BMP`, cropped to 387×161 above its warning text), then the scan of the CD label (of the August CD of the same release) | 127×162 / 387×161 / 750×734 |
| `screams` | the installer's title art in `SETUP.EXE` (bitmap 7500, 350×179, cropped to 350×119 above its copyright block); no box or label scan exists online | 350×119 |
| `disney` | the box front from Berkeley Systems' 1997 product page (`box.disneyL.jpg`, through the Wayback Machine), then the installer splash (`SETUP.BMP`, cropped to 387×172 above its warning text) | 128×162 / 387×172 |
| `farside` | the Internet Archive's photo of the box beside another, in the item of the damaged floppy images (`far-side-software-v0-z46qj0d0a2fc1.webp`, cropped to the box at 546,122, 408×508); a WebP, decoded where Windows has its WebP codec, else the generated cover; nothing on the disks (the installer's picture is SZDD-compressed) | 408×508 |
| `dilbert` | the Internet Archive's photo of the box front (`dilbert_screensaver_collection/box.jpg`, the flat ZIP's item; cropped to the box at 100,215, 965×1315, a previous owner's handwritten name on it kept), then the installer's picture on disk 1 (`INSTALL.BMP`, Dogbert, art `panel`, "Setup art") | 965×1315 / 63×123 |

The box fronts are small (a tile is never shown larger than 160×200 px),
but they are the retail boxes; a disc label or an installer splash is what
stands in when they can't be fetched. Wayback Machine URLs are the `id_`
form, which serves the archived file's own bytes (the md5 is of those).

The registry holds only URLs, md5s, sizes, paths and crops: no picture is
shipped. Downloads are HTTPS, checked against their published md5 and size
before use (a wrong file is deleted and the next source tried), and kept in
`<download dir>\covers\` (`--download-dir`; by default
`<data folder>\downloads\covers`, taken only once a download starts), where a later import or
refresh reuses them without a request. A disc source is read from the source
being imported (only the named file; with its md5 checked, so another pressing
is skipped); a bitmap resource of an NE or PE file gets a `BITMAPFILEHEADER`
before it is decoded. The Simpsons art belongs to Fox, Star Wars Screen
Entertainment's to Lucasfilm and Star Trek's to Paramount; the Looney
Tunes' to Warner Bros., the Disney Collection's to Disney and Marvel's to
Marvel (with Berkeley's): each is fetched onto the user's machine at import
time and never bundled.

**During an import** a `cover` phase comes between `verify` and `finalize`
(`Progress::Phase::cover`, "Getting the cover art"). It tries only the sources
better than the stored original (a re-import fetches nothing it already has,
and a better source replaces a fallback). Each download may take two attempts
with a 15-second timeout; after a network-class failure (DNS, connect, TLS, a
timeout) the remaining downloads are skipped and the disc art is used.
`--no-cover-download` skips the downloads altogether. The files are staged in
`covers\<id>.importing-<pid>` and moved in with the package swap (Deluxe
included). **A cover never fails an import**: when every source fails, the
cover stays as it was (or generated), a log line says so, and `cover.json`
records what was tried. `user.png` is never touched by an import, and
`--remove` keeps `covers\<id>` for a later import.

**On disk**, under `<win>\covers\<id>\`: `original.png` (decoded with WIC, EXIF
orientation applied, cropped, long side capped at 2048 px, RGBA), `user.png`
(your picture, normalized the same way), `tile.png` (640×800, opaque) and
`cover.json` (version 1: where the original came from with its md5s, your
picture's file name, the tile's renderer version, the last attempts). Every
file is written as `<name>.tmp-<pid>` and renamed; a stored file counts only
while its md5 matches `cover.json`. The recovery sweep deletes
`covers\*.importing-*` and `covers\<id>\*.tmp-*`.

**The tile** (`cover_image.h`; pure functions, so the tests pin them): a disc
label becomes a circle 552 px across, centred at (320, 368), with an
anti-aliased rim, on the night gradient `#262B4F` → `#12152A`; any other
picture within ±15% of 4:5 fills the tile, and anything else is contained on
bands of the mean colour of its two outermost rows (or columns), with
transparency flattened onto them. A picture of 256 colours or fewer scaled by
2 or more is scaled up by a whole factor with nearest-neighbour first, then
with the cubic resampler. Raising `kRendererVersion` makes the next catalog
write render every stored tile again.

```
adimport --set-cover <id> <picture> [--dest <root>] [--quiet]
adimport --clear-cover <id> [--dest <root>] [--quiet]
adimport --refresh-covers [<id> | all] [--force] [--dest <root>] [--download-dir <dir>] [--quiet]
adimport --gui --change-cover <id> [--dest <root>] [--download-dir <dir>] [--no-cover-download]
adimport --gui --refresh-covers [<id> | all] [--force] [--dest <root>] [--download-dir <dir>]
```

* `--set-cover` takes any picture Windows can read (PNG, JPEG, GIF, BMP, TIFF,
  ICO, JPEG XR; WebP, HEIF or AVIF when their codecs are installed; 32 to
  16384 px a side, at most 64 MB): exit 0, 1 (usage, not imported, the lock
  held, local I/O) or 2 (the picture can't be read). It stays on this
  computer; only its file name is recorded.
* `--clear-cover` goes back to the original: exit 0 (also when there is
  nothing to clear, which writes nothing) or 1.
* `--refresh-covers` (every installed release when no id is given) tries the
  downloads better than the current original, and repairs missing or damaged
  files: exit 0 when every release has the best cover this run could reach, 4
  when a download failed (the previous cover is kept; the output says which),
  1 on an error. `--force` tries every download, and keeps the stored
  original when none works. Nothing fetches covers in the background.
* `--change-cover` opens only the GUI's cover window (`gui/README.md`).
* `--gui --refresh-covers` is `--refresh-covers` in a progress window, then
  a page saying what it got: exit 0 when a cover changed, else the first
  failure (4 when a download failed), else 5. It is what the settings
  dialog's **Get the covers** link runs, and the importer's Sources page has
  the same action for the releases still showing a generated cover.
* `--list-packages` adds `; cover: <origin> (<label>)` to each installed line.
* The cover commands are modes, like `--catalog-only`: one at a time, each
  with only its own options; `--no-cover-download` belongs to imports.
* The library calls are `covers.h`: `cover_info`, `set_cover`, `clear_cover`
  and `refresh_covers` (each takes `import.lock`, writes atomically and
  rewrites the catalog when anything changed).
* `AD_COVER_DOWNLOAD=0` in the environment turns every cover download off, as
  `--no-cover-download` does (tests whose command lines are fixed use it).

**Installs made before covers** have no `covers\` and show generated covers
until `adimport --refresh-covers` (downloads) or a re-import (disc art too).
The settings dialog offers the refresh itself: while any release shows a
generated cover, **Get the covers** appears under the strip's status line
(and the importer's Sources page says how many releases have no cover yet,
with the same button). Only the 3.2 installer splash needs a re-import from
the disc; a refresh gets that release's disc-label scan.
To use your own pictures instead, for example the box photos kept with the
research notes:

```
adimport --set-cover deluxe "<repo>\research\win\pkg\covers\deluxe\cover_supplied.png"
adimport --set-cover tt     "<repo>\research\win\pkg\covers\tt\cover_supplied.png"
adimport --set-cover ad10   "<repo>\research\win\pkg\covers\ad10\cover_supplied.png"
```

## GUI

`adimport --gui` (and `adimport` started from Explorer with no arguments)
shows the themed windows in `gui/` (`adw_import_gui`; COVERS.md §4). They
are documented in [`gui/README.md`](gui/README.md): the pages (Sources,
Downloads, Progress, Result, Cover), their command ids, the exit codes and
the test hooks (`AD_IMPORT_TEST_SCREENSHOT` / `_STATE`, `AD_IMPORT_TEST_PICK`).
`adimport.cc` hands them a `gui::Request` (`gui/gui.h`) and returns
`gui::run`'s result as the exit code: 0 when anything was imported or a
cover changed, else the first failure, else 5 (cancelled, nothing
changed). `--change-cover <id>` opens only the cover window, and
`--refresh-covers` only a progress window over the cover downloads.
`--download-dir` applies to the windows as well: where the Downloads page
looks for "already downloaded", where downloads and cover pictures go.

Started from Explorer with no console, it behaves as `--gui`. The
manifest's `consoleAllocationPolicy=detached` (Windows 11 24H2+) keeps a
GUI parent from giving it a console window; on earlier Windows the parent
should pass `CREATE_NO_WINDOW`, as LongAfterDark.scr's settings dialog does (it
reads exit 5 as "cancelled, nothing changed"). `AD_GUI_AUTOCLOSE=1` skips
the final result page (tests).

## Tests

`import.md5` (RFC 1321 vectors, streaming, files; its scratch folder is
removed), `import.iso` (synthetic
images from `tests/iso_builder.h` in seven layouts, each also through the
sector-aligned read path a raw CD volume uses, plus Joliet directories
paired with their 8.3 twins by shared file data), `import.zip`
(`tests/zip_builder.h`: round trips, the check byte incl. flag bit 3, wrong
passwords and check-byte false positives, bad CRC, damaged data, size
mismatch, truncation, ZIP64, multi-disk, strong encryption, unsupported
methods, zip-slip and device names, duplicates (non-ASCII case too); member
names as UTF-8 — code page 437 names without bit 11 decoded (u-umlaut and
o-umlaut two names, u-umlaut and U-umlaut one), UTF-8 without it kept, a
byte that is not UTF-8 under it read as U+FFFD; the
password candidates and their order, decoys, a check-byte trap, no
password, two archives under different passwords; the disk-set names of
`ZipNames::disk_folders` — `DISK<n>/` folder entries, the bare names in
them, any case, gaps in the numbering, and every refused shape: another
folder, a deeper path, a folder entry with data, a file beside the disks),
`import.arj`
(`tests/arj_builder.h`: stored round trips
— one member, many, an empty one, one over 64 KiB, names found without case,
code page 437 names, 7-bit text members; every damaged or foreign header
shape refused with its message — CRCs, sizes, flags, versions, methods, file
types, hostile names, truncation, the end marker, extended headers,
duplicates (code page 437's letters folded as Windows folds them), more
than 65,535 members; a member split over three volumes joined, every joining
rule broken, a flipped byte naming the second volume; thirteen fixed
method-1/method-4 vectors — made-up data from the research encoder
(`arj_vectors.py`, `arj_vectors_more.py`: text, every byte, overlapping and
far matches, the window's edge, two blocks, 100,000 bytes through the 64 KiB
ring) and one block from explicit tables whose NT, c and p codes are longer
than their lookup tables (the three tree walks), each decoded to the same
bytes by the Python reference and 7-Zip — through the decoders and through
archives, methods 1–3 alike, each cut by 1–3 bytes, every single-bit flip
(sampled in the two 100,000-byte ones; a clean error or, for a padding bit,
the original bytes; never more than the recorded size), one size larger,
one smaller for methods 1–3, and every smaller size of a two-block and a
method-4 vector (the prefix, or an error: nothing after the last needed
symbol is read); streams crafted bit by bit that break each decoder bound —
an empty block, table sizes, a 17-bit code length, zero runs past the
table, incomplete and over-subscribed codes, constant symbols out of range,
a match before the start or beyond the window — blocks that end on the
32/64 KiB edges, every flip inside a 16-bit NT code; an LZH member split
over two volumes, mixed methods, members over 64 KiB; the volume names; no
encoder: the decoder is a modified version of UNARJ's, for programs that
are not ARJ archivers), `import.szdd`
(`tests/szdd_builder.h`: round trips including the window's initial spaces,
overlapping runs, a 1-byte, an empty and a 70 KB file; two fixed vectors
that Windows' EXPAND.EXE expands to the same bytes; the header — the magic,
KWAJ, mode 'B', a short header, the missing character; data cut short, a
match past the size, bytes left over), `import.kwaj` (`tests/kwaj_builder.h`,
which compresses nothing: eight fixed vectors — made-up data the research
encoder wrote, and six streams written token by token: 16-bit codes,
symbols without codes, MATCHLEN2 symbol 0, distance 4096, a 70 KB file in
two chunks, the disks' padding quirk, MATCHLEN2 after a run of 31 literals
and MATCHLEN after 32 — pinned by their md5s, each expanded
to the same bytes by the research reference decoder, libmspack and Deark;
zero padding that reads as a whole token; literal-only files round-tripped;
every refused header — SZDD, another magic, a short file, methods 0/1/2/4,
the header flags, another data offset; the tables — the sixth nibble, types
out of range, a 17-bit length, incomplete, over-subscribed and empty codes,
streams cut inside them; the end rule — a token cut 8 or more bits before
the end refused, a cut on a token boundary giving a clean prefix, the
padding's partial token producing nothing — a match, or a literal run cut
after its first literal (none of its literals); `max_size` at the size, one byte
short and 0, refused as `KwajTooLarge`; chunks of at most 64 KiB; every
single-bit flip of the vectors of 1 KB or less, 35,416 of them: an error,
other bytes, or (a flipped padding bit) the same bytes; never a crash or
more than the bound), `import.isz` (`tests/isz_builder.h`, which lays out
given streams as libraries and split sets and writes DCL token by token,
compressing nothing; `tests/isz_vectors.h`, generated by the research
script `research/win/pkg/more/i1/gen_isz_vectors.py`: 20 of the format
survey's libraries that InstallShield's own `ICOMP` made from data the
tests' own generators produce — all 16 of 1 KB or less and four larger —
each decoded to that data, the stored one refused; the survey's 20 crafted
and damaged streams, rewritten byte for byte by the builder and decoded as
its reference did, but for the three this reader refuses by name; the
header, lengths 2–518, every distance at dictionary bits 4, 5 and 6, a copy
reaching exactly the first byte and one byte further, the size rules, a
cut at every byte, bytes and one-bits after the end code, chunks over 64
KiB; split sets with the boundary at every byte of a member and three
volumes in all six orders, volumes under each other's names or missing,
and a set under swapped names, which decodes; every container rule broken
and every refusal by name, each with its message; every single-bit flip of
the small libraries and streams, 62,104 and 26,456 of them: a clean error
or exactly the recorded size, never a crash), `import.fat` (`tests/fat_builder.h`: 1.44 MB, 2.88 MB,
FAT16 and 1 KB-sector volumes, a fragmented file, subdirectories, skipped
entries, a volume label in code page 437 (and one whose leading 0xE5 is
stored as 0x05), every refused BPB, loops, free/bad/out-of-range clusters, short
chains, each read from the file and from memory alike; content sniffing;
the union of two floppies; directory keys, an aliased subdirectory, and a
union's keys; ZIPs of floppy images — stored and deflated images with a
scan, an image one byte too long, a floppy-sized member with no boot sector
and one with a boot sector but no FAT volume beside them (ignored), a 720 KB
one, a single image, a password-protected image and a damaged one refused,
damage past the first 64 KiB of a member with no boot sector never met,
images past the bound refused while members with no boot sector do not
count, and a flat ZIP whose floppy-sized member is no FAT volume read as
install files), `import.import`
(Deluxe image/folder imports byte-compared, import.json parsed with phosg,
atomic failure cases, hostile and device names, one folder listed twice
(siblings, a loop and a 3^12 bomb, in both trees; a folder junction), the
staging budget, a size mismatch refused before the copy, lock, late cancel, recovery
from both interrupted-swap states, an unrepresentable timestamp,
`AD_ASSETS_DIR` trimming, manifest shape; `utf8.h`'s UTF-8 test against
Windows' strict decoder over every string of one and two bytes, every
three-byte string that starts past 0xBF and four-byte strings around every
edge, its repair, and `json_escape` read back by phosg as UTF-8 for every
string of one and two bytes and random ones; disk sets as ZIPs, folders,
ISO and FAT images — one name on two disks listed with another size or read
with other bytes, a root holding something beside its disks read as it is
(a folder, an image) or refused (a ZIP), a ZIP's disks flat — and then
imported end to end: an AD 3.x install over two disks, a Microsoft Setup
one, a Presage one over five and a plain CD tree split in two, one disk
alone, and a note beside them never read), `import.packages`
(`tests/pkg_fixture.h`: all fourteen releases as folders, ISOs and FAT images,
split floppies in either order — Star Wars Screen Entertainment also as its
five 1.44 MB floppies and a flat ZIP, from a made-up Presage install with
stored and LZH members cut across ARJ volumes, SZDD loose files and decoys;
Star Trek: The Screen Saver as its two floppies in either order, the ZIP of
them (disk 2 first, a scan beside them), a flat ZIP, an ISO and a folder,
from a made-up Microsoft Setup install of KWAJ files — its module names
with After Dark 2.0's leading space, its About texts with the stand-in
line and hand-wrapped breaks — and decoys; the Looney Tunes, ScreamSavers
and the Disney Collection as made-up AD 3.x installs with their real
`MODMISC.ZIP` and `AFI.ZIP` member names (and the ad32 fixture's now hold
3.2's real ones, `AD30RSDB.DLL` among them), as folders, flat ZIPs,
`DISK<n>` folders and ZIPs, and floppies, with an owner's-note decoy each
and Disney's `BEAUTYOL.ZIP`, locked in folder sources and never opened;
The Far Side and Dilbert as made-up installs of Delrina's installer (plain
and SZDD loose files, version stamps, decoys and a bulletin board's note):
as a folder, a flat ZIP, `DISK<n>` folders loose and zipped, floppies in
any order and a ZIP per disk known by md5 (verified `image`), the decoys
never opened, disk 1 alone ("missing DISK2…"), any other disk alone or disk
1 without the installer, the marker or its tag (no release), Star Wars
Screen Entertainment never taken for either nor either for it, a damaged
version stamp, the I1 and I3 failures, both in one folder (ambiguous) and
their catalog entries; a
ScreamSavers source never taken for After Dark 3.2 nor 3.2 for it, disk 1
alone (ScreamSavers': "needs every install disk"), I4 over `*_SOUND.DLL`,
the Disney names and ScreamSavers' `screen`; Marvel Comics Screen Posters
and Snoopy's Screen Savers as made-up InstallShield 2 installs (a
`SETUP.PKG`, split and whole libraries of streams written token by token,
undecodable decoy members in the libraries the table does not read) as a
folder, a flat ZIP, `Disk1`/`Disk2` folders and a ZIP of them, and two
floppies in either order — the `from` forms, DOS times, `screen` and
`needs`; a known ZIP md5 verified `image`; disk 1 alone ("needs every
install disk") and disk 2 alone (no release); decoys, notes and the Looney
Tunes' `SETUP.PKG` locked and never read; a `SETUP.PKG` without the tag
member, of 70 KB, of another magic, with a bad offset, a group one byte
short, cut short or with a stray byte (no release, no crash); `Disk1`/`Disk2`
folders beside a `desktop.ini` (no release, `identify_folder`'s reason
starting with the note that says why); `--package`
mismatches; a volume naming a locked `IMAGES.3`; `IMAGES.2` cut short,
junk `MODULES.LIB`, another set's `IMAGES.2`, the tag member missing, a
stream with dictionary bits 7, a crossing member without its end code (2);
a flipped literal (3); the staging budget; a cancel; a missing member
(`partial`); both releases' volumes swapped (their own message, nothing
copied); and the islib invariants;
identification, unknown, ambiguous, `--package`, image md5s, Presage's
`INSTALL.DAT` (another short name, a 70 KB one, no first archive beside it),
`SETUP.LST` (another title, one in capitals, a 70 KB one, no `MISSION.AD_`
beside it); known disk sets (either copy of each disk in any order and form
verified `image`, also disk 1 followed by the ZIP of both, whose disk 1 is
then ignored as the same image and logged; disk 1 alone, disk 2 alone, both
copies of one disk, a disk with a stranger; the set with a stranger, disk 1
with both copies of disk 2, and both at once verified `files`, logged as
every install disk and other images besides; `--package`, a disk of one
release with another's image, disk 2 when no md5 is known; the ZIP of both
under code page 437 names, bit 11 clear — `DISKüö1.IMG` and `DISKüö2.IMG`,
and `TREKü.IMG` beside `TREKö.IMG`, names that differ only in a letter
outside ASCII — verified `image` with the names decoded, and a disk with a
code page 437 volume label, each record strict UTF-8);
every install disk needed; exact file sets and `from` forms, fix-ups only
from matching sources, each invariant (the intermission and ad2kwaj
recipes' own too), the FAT times kept on KWAJ copies,
required files and archives, the password never written or logged, the
owner's notes and swse's and startrek's decoys never read (locked in a folder source), the
skipped ARJ members never decoded (they are damaged), a volume chain that
points past the registry's archives refused with the volume it names never
opened (a locked `SWSE2.A04`), another `.ARJ` skipped and never opened;
damaged ARJ volumes and SZDD files (2, or 3 for an SZDD literal: no
checksum), damaged KWAJ files (2 for a table type, a header flag, an SZDD
file, tables or a token cut short; 3 for a changed literal, and for a file
cut to a clean prefix, whose size the manifest refuses before a byte is
written), names that differ only in a non-ASCII letter's case (two ARJ
members, two ZIPs' members), the staging budget (with KWAJ's expanded
sizes) and a planned size off the manifest refused before a byte is written; per-package atomicity,
failed and cancelled re-imports, the lock; recovery from every interrupted
swap and removal; `--catalog-only` without `FILES`, `--remove`,
`--list-packages`; a source missing a known file verified `partial`, a known
file of another size refused before the copy; the merged catalog's ids, order, names, overrides,
`sameAs`, `packages` (`startrek` first), `screen` on `startrek`'s, `marvel`'s and `screams`' entries alone, After Dark 2.0's About
rules, and Deluxe's entries unchanged; `adimport.exe`'s
options and exit codes; the registry's cover sources — at least one per
package, HTTPS URLs with 32-hex md5s, sizes and unique file names, disc
paths, crops, and the §2.3 decisions), `import.covers` (offline: synthetic
pictures written through WIC, synthetic registries and the loopback server;
the tile rules — fill or contain on both sides of ±15% of 4:5, band colours,
the disc's circle and anti-aliased rim, nearest-then-cubic, crops inside and
outside, all eight EXIF orientations, the 2048 cap; decoding BMP, PNG, JPEG,
GIF and TIFF, a JPEG with EXIF orientation 6, NE and PE `RT_BITMAP`
resources, 16×16, random bytes and a file over 64 MB refused; downloads
checked by md5 and size through a redirect, 404 and a wrong md5 falling
through to the next source with the bad file deleted, a stalled server timing
out with the other downloads skipped and the disc art used, reuse from
`<download dir>\covers` without a request, `--no-cover-download`, a disc file
of another pressing; during an import `covers\<id>` and `packages[].cover`,
a cancel in the cover phase, every source failing with exit 0 and the same
`import.json`, a re-import fetching nothing and keeping `user.png`, a better
source replacing a fallback, `--remove` keeping the cover, the recovery
sweep, Deluxe's `FILES` byte-identical; `set_cover`, `clear_cover` and
`refresh_covers` — not imported, unknown, an unreadable picture, the lock
held, `changed`, no catalog write for nothing to clear, disc → download once
the server answers, `--force`, a damaged `original.png` fetched again, a
missing tile and a stale renderer rendered again by `--catalog-only`, a
damaged `cover.json` listed as generated until refresh, an unreadable or
damaged one reported by `cover_info` (`CoverInfo::error`), numbers out of
range in it read as absent, bitmap resources with a damaged colour count
refused — and `adimport.exe`'s
exit codes for the three commands), `import.download` (loopback server: redirects, drop
+ resume, 416, Range ignored or botched, md5 mismatch, 404, refused, cancel,
more drops than `max_attempts` with progress, the destination lock; the
redirect policy (no https to http); a body with no length past `max_size` or
the published size; reuse and resume only for the recorded URL; a cancel
token stopping a download blocked on a silent server),
`import.cli` (exit codes, `--md5` validation, `--catalog-only`,
`--download` against the loopback server, a URL named after a device; the cover modes' exclusivity and
options, `--no-cover-download` only with imports, `--change-cover`'s
companions, the `--list-packages` cover column and a floppy set's download
size ("download 2.8 MB (2 floppy images)"); `AD_GUI_TESTS=1` also drives
the progress window, the source chooser and its Internet Archive list, back
and out), `import.pkg_download` (every package from its registry copies on a
loopback server that redirects like archive.org: the five disc images
verified `image`, the Simpsons as a flat ZIP verified `files` (and swse's
flat ZIP once its image copy is gone), Star Trek's two floppy images
verified `image` as a known disk set, the ZIPs of Marvel (both copies),
Snoopy, the Looney Tunes, ScreamSavers and the Disney Collection verified
`image` (each a known image, its medium starting "ZIP"), the ZIP as
`--image`, nested and password-protected ZIPs refused; fallback after a 404,
a wrong size — refused before a byte is written, the `.part` resumed by the
next copy — and a wrong md5; a floppy set's progress over both images, its
second image missing or damaged (the other copy used), both images reused
without a request, one already on disk, the other copy's complete set
preferred to a lone first image; every copy failing (3 or 4, nothing left
behind); reuse of a file from any copy without a request, a stale file of
the wrong size fetched again; no copy known; `--url` with and without a
package; `--md5` over the registry, and over a floppy set's copies (each
copy's first image's md5 only: the other copy's disk 1 named moves on to
that copy; the second image still checked against its own); the download record in both
`import.json` versions; `import_downloads` over all fourteen and a cancel
mid-way; the built-in copies' shape — archive.org URLs, image md5s equal to
the known images, file names per content, the Simpsons ZIPs' md5s, swse's
ISO, Redump BIN and ZIP, Star Trek's copies each a complete set of its
known disks; and
`adimport.exe`'s `--download <id>`/`all` parsing and `--list-packages`
sizes), `import.catalog` (the RTF, text,
STRINGLIST and control-record readers against hand-made inputs, whole
synthetic PE32/NE modules from `tests/module_builder.h` with multi-language
resources, NE modules told apart by their exports as the ne16 lane does —
`MODULE` winning over `SAVERINIT`/`SAVERDRAW`, lower-case export names, names
with no entry-table entry, no `SAVERDLGPROC`, an IMX-shaped `.AD`, and every
file the lane refuses left out with its reason (`SETCURRSAVER`, `IMXX_`
files, one export alone, `SAVERMAIN`, none) — a FILES tree with junk and
duplicate ids, a package tree with `*.AD` and `*.IMX` together (Deluxe's
places never list `*.IMX`; `WINDOWS` is never scanned; refused files logged)
and the registry's names, the JSON layout (`abi` last, only on IMX entries;
`screen` last, only on its package's), After Dark 2.0's About rules
(`ad20_about`: the stand-in dropped only as the last line, a break joined
only after a space and before a lower-case letter) on a Classic module of
`startrek` with its `screen`, and the same file as written, with no
`screen`, in every other package,
and `--catalog-only` as a library call incl. the lock), `import.catalog_real`
(skipped, exit 77, unless both the imported assets and the prototype's
`research/win/catalog-win.json` are present: the generated catalog, through
the library and through `adimport --catalog-only` on a scratch copy of the
modules, must equal the prototype's field by field, the STARRYNI erratum
and the PACKAGES.md §6 fields aside).

Opt-in: `import.covers_real` (`AD_E2E=1`) fetches every registry cover
download into a scratch folder, checks its md5 and size, decodes, crops and
renders it; with `AD_E2E_PKG=1` it also imports the real images (by md5, from
`AD_SOURCE_ISO_DIR` — `;`-separated folders — or `<repo>\source_iso`, and the
folders directly in each; the Deluxe ISO also from the downloads folder,
which is only read) with `--no-cover-download`, so every disc source is
extracted, and checks the originals' sizes (387×183, 387×172, 387×204,
118×226, and the later releases' 79×175, 387×161, 350×119 and 387×172); a
release with no cover source on its disc (Star Wars Screen Entertainment,
Star Trek: The Screen Saver, Marvel Comics Screen Posters) is skipped. It writes every tile side by side into
`<build dir>\covers-sheet.png` for a person to look at, and deletes its
scratch tree unless `AD_E2E_KEEP=1`. Every other opt-in import runs with
`--no-cover-download`. `import.download_real` (`AD_E2E=1`) runs `adimport --download <id>`
for every package into a scratch downloads folder and a scratch root under
the build tree. A local file with a copy's published size and md5
(`AD_E2E_LOCAL_DIRS`, `;`-separated; default the image folders above,
the installed data folder's `downloads` and its `verify\`) is hard-linked
or copied in first, so what can be verified locally is not fetched again
(`AD_E2E_NO_SEED=1` fetches everything); the importer still checks its md5.
Each import must exit 0 with kind `download`, the expected `verified`,
nothing missing, every file a manifest match and 84/46/44/13/15/14/16/1/8/12/15/16/14/16
modules. Every copy with other bytes than a package's first (another file
name: the Simpsons' second ZIP, swse's Redump BIN and flat ZIP, Star Trek's
second pair of images, Marvel's second ZIP, Dilbert's four disk ZIPs) is fetched with `--url`/`--md5` — a floppy set's images,
or a set of disk ZIPs,
with the library's `download`, then imported `--image` each — and imported
the same way (`verified: image` for a known image or disk set, else
`files`). `AD_E2E_PACKAGES=<id>[,<id>…]` limits every step to
those packages. Every registry URL, fallbacks and every image of a floppy
set included, must then answer a Range
request with the published size and the same bytes as the verified file
(its last 64 KiB, and an ISO's primary volume descriptor); a file the
Internet Archive serves from inside a ZIP (The Far Side's disk ZIPs,
Dilbert's second copy), which ignores ranges, is fetched whole instead and
must be the md5's bytes. The scratch tree
is deleted afterwards unless `AD_E2E_KEEP=1`.
`import.e2e` (`AD_E2E=1`) downloads the Deluxe image into
`<scratch>-downloads` (or `AD_E2E_DOWNLOAD_DIR`; the user's own download in
the installed data folder is linked in first when it is there), imports it
through `--download`, `--iso`, `--from` the imported tree and `--from` the
image mounted by Windows, checks every file against the manifest, and
byte-compares the seeded tree at the installed data folder's
`assets\win\FILES` (`AD_E2E_SEED`).
`import.isz_real` (`AD_E2E_PKG=1`) finds the user's Marvel and Snoopy ZIPs
by size and md5 (in `AD_SOURCE_ISO_DIR` or `<repo>\source_iso`), reads them
as disk sets (Marvel's union has 14 names, Snoopy's 13: `DREAM.ON` is on
both disks, one file), and checks all 88 members of their libraries against
the InstallShield survey's md5s (`tests/isz_real.h`, generated: names,
sizes and md5s only), from the ZIPs and again from folder copies of the
library volumes; it prints the union's count, never its listing, so no
previous owner's note is named.
`import.pkg_real` (`AD_E2E_PKG=1`) finds the five package images, Star
Trek's two disk images (loose, or in the ZIP they came in:
`test_util.h` `find_disk_set`), the known ZIPs of Marvel, Snoopy, the
Looney Tunes, ScreamSavers and the Disney Collection, The Far Side's five
disk ZIPs and Dilbert's four (its flat ZIP too, which must give the same
files), by size and
md5 in `AD_SOURCE_ISO_DIR` (`;`-separated folders; default
`<repo>\source_iso`) and the folders directly in each (so
`source_iso\Implemented` too), and imports each into a fresh scratch root
(exit 0, `verified: image`, nothing missing, the installed files exactly the
manifest, the §4.3 counts, the invariants — the intermission, ad2kwaj and
islib recipes' own — and 46/44/13/15/14/16/1/8/12/15/16/14/16 modules in the
right lanes, no owner's note named; farside's and dilbert's as ASA and IMQ
entries, `SAVERMAIN`, one button each; swse's as IMX entries with their
registry names and one button each; startrek's as Classic entries with
`screen`, trimmed names, the Planetary Atlas override, After Dark 2.0's
About rules and their two buttons; marvel's with its `screen` and two
buttons; snoopy's eight with 27 controls; screams' with `screen`; disney's
with its five names spelt out), imports the Looney Tunes' `LOONEY_T` CD
when an image folder holds it (verified `image`, the ZIP's files), checks
Marvel's and Snoopy's baked tables against their disks, read from the ZIPs
(each row's member in its library with the manifest's size; every member
the table leaves one the recipe never installs; `SETUP.PKG`, parsed on its
own, listing each library the table reads with exactly its members and
sizes, and the tag member in the tag library), imports their disks copied
out of the ZIPs (the owners' notes skipped by name, never read) as a flat
folder, `DISK1`/`DISK2` folders and a flat ZIP (verified `files`, the ZIP's
files) and each disk alone (2), mounts the 10th
Anniversary, Totally Twisted and Star Wars Screen Entertainment CDs with
Windows and imports `--from` the drive (the same files as the image; skipped
when mounting is refused), checks swse's real `INSTALL.DAT` against the
recipe (the loose files are its lines 4, 10–13 and 37; every other line is an
archive, another MIDI set or a file the recipe never reads) and Star Trek's
real `ST_NSTLL.INF`, KWAJ-expanded from disk 1 (each of the recipe's 27 rows
is an INF line with the same disk, installed name and size, placed where its
section installs it; the disk tag files are the INF's; each of the other 34
lines is `SETUP.LST`, read only to identify, or one of the 33 files the
import never opens, as it never opens the INF itself), imports Star Trek's
two real disks zipped under code page 437 names with bit 11 clear, as
Explorer or 7-Zip on an English Windows zips them (`DISKüö1.IMG` and
`DISKüö2.IMG`; `TREKü.IMG` and `TREKö.IMG`: verified `image`, the same files
as the images' import, an `import.json` that is strict UTF-8 with the names
decoded), then all thirteen
plus Deluxe `--from` the installed assets (read only; `AD_ASSETS_DIR` picks
them, e.g. `<repo>\build\win-pkg-setup\assets`) into one root (314 modules,
232 over the first seven, 73 `sameAs`, unique names per lane, the releases
oldest first, every Deluxe field as the installed catalog has it), and
checks that re-importing each package changes nothing else.

**Before a release** (these stay opt-in: they need the real images, the
network or both, and take minutes), in a Release build directory:

```
AD_E2E_PKG=1 ctest -R "import\.(pkg_real|covers_real)"          # the images in source_iso (and its subfolders)
AD_E2E=1 ctest -R "import\.(e2e|download_real|covers_real)"      # the Internet Archive copies (~800 MB)
ctest -L gui                                                     # real windows on the desktop, briefly
```

Every one must pass (or report skipped, 77, for a missing image), and none
leaves anything outside its scratch tree under the build directory.

**No test touches the user's data folder.** Every suite first points
`AD_LOCALAPPDATA` at `<scratch>\localappdata` (`tests/test_util.h`
`sandbox_data_root`), so its own defaults and those of every `adimport` it
starts (some run without `--dest`) resolve there, and nothing is written
to the real one. The suites that read the user's installed data
(`catalog_real`, `e2e`, `pkg_real`, `download_real`, `covers_real`) find it
before that with `installed_data_root()`/`installed_assets_root()`, read
only. `import.import` checks that `AD_ASSETS_DIR` alone is the assets
root, and that the defaults are `<AD_LOCALAPPDATA>\LongAfterDark\…` (trimmed,
with a trailing separator tolerated), else `<LOCALAPPDATA>\LongAfterDark\…`
when `AD_LOCALAPPDATA` is blank, without creating anything; `import.cli`
runs `adimport` over a scratch base: a listing names the default assets
root and creates nothing, explicit locations (`--dest`, `AD_ASSETS_DIR`,
the cover commands, an import without cover downloads) leave the data
folder alone, and a download without `--download-dir` lands in
`<data folder>\downloads`. `import.gui_flow` walks Sources and Downloads
with no `--dest` and checks that nothing is created in the data folder.

The tests delete their scratch trees with `remove_tree` (`winutil.h`), not
`std::filesystem::remove_all`, which this toolchain's libc++ makes about a
thousand times slower on Windows.
