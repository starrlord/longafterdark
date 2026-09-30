// What the InstallShield survey found in the libraries of the user's
// Marvel Comics Screen Posters and Snoopy's Screen Savers ZIPs
// (research/win/pkg/installshield/expanded/{marvel,snoopy}/inventory.json:
// every member's md5 agreed across the survey's reference, ICOMP.EXE, Deark,
// zlib's contrib/blast, isdecomp, idecomp and stix-unpacker), written by
// research/win/pkg/more/i1/gen_isz_vectors.py for test_isz.cc's opt-in real
// test: names, sizes and md5s only, as the known_files_*.inc manifests hold.
#pragma once

#include <cstddef>
#include <cstdint>

namespace test {

struct IszRealFile {
  const char* name;  // a volume: its path in the ZIP; a member: its name
  uint32_t size;
  const char* md5;
};

struct IszRealLibrary {
  const char* zip_md5;          // the ZIP it is in
  const char* logical_name;     // as the installer's SETUP.PKG names it
  const IszRealFile* volumes;   // in set order
  size_t volume_count;
  const IszRealFile* members;   // in table order
  size_t member_count;
};

struct IszRealZip {
  const char* release;
  const char* name;
  uint64_t size;
  const char* md5;
};

inline constexpr IszRealFile kMarvel0Volumes[] = {
    {"Disk1/IMAGES.1", 1085239, "944876dd52516676245a54472bf595b8"},
    {"Disk2/IMAGES.2", 383849, "7f3e31c713081eaf7b7e5a67f191169c"},
};
inline constexpr IszRealFile kMarvel0Members[] = {
    {"AV2.FTT", 4122, "3e7d71fa0d827367e0a4df06643e0863"},
    {"AVENGE#4.FIF", 27170, "c2ce2ad3932c47603723daac69855e9a"},
    {"CABLE.FIF", 39419, "13ced1bbb900f0045242e928b820bb39"},
    {"CABLE.FTT", 4122, "bf4ef0e2dc2a512b3b9c38d5bdd9d746"},
    {"CAPT.FIF", 40082, "2ae3b7caea60448bc322a9a575c136cb"},
    {"CAPT.FTT", 2074, "6f41740367826dd98e4e23c2b95eb25b"},
    {"CAPTAINA.FIF", 20037, "29cf0197d26d09e07f205deb43787bff"},
    {"CAPTAINA.FTT", 4122, "b695c3e1e9f32c4805dbbd86443f4fd5"},
    {"CAPTAINW.FIF", 40893, "06851eb4251421509e40445092ae48cf"},
    {"CLOBBERI.FIF", 23248, "c3a6c81d83bdf6f7cedaafa222103d82"},
    {"COVER.FTT", 4122, "ab47c15ba9ffe3edb859d0a000c14146"},
    {"DAREDEVI.FIF", 27446, "c198b8d2e03d626759e5c986f314e13c"},
    {"DAREDEVI.FTT", 4122, "4a479969f4bcc071801aa9f9ff0153db"},
    {"FAN4#1.FIF", 28063, "c51587b6d1bb665a7963e5bf2c88f749"},
    {"FAN4#1.FTT", 4122, "cb7305092976b1c47c911acf82374aa4"},
    {"FANT0A.FTT", 4122, "95e2b31dd9d2c56ab17d5abe633ef1c4"},
    {"FANT0WFB.FIF", 26560, "5f1a108d32d53cca608bb51609ef264b"},
    {"FANTAST0.FTT", 4122, "be1590601bcf32b731e409d325863844"},
    {"GAMBWOLV.FIF", 44955, "5beb46856152165baf2e685fe9bc80cd"},
    {"GHOST.FTT", 1306, "715e249b2cb47aac2b64771e59f152be"},
    {"GHOSTID.FIF", 14780, "2312280d285da8223a64d2271409cc28"},
    {"HULK#1.FIF", 28824, "7c86f778d663e5c2091f41fc7a5cb41a"},
    {"HULK#1.FTT", 4122, "43dee1027149ec8893855bad0b84a953"},
    {"HULKFACE.FIF", 14085, "210f85d785eca989ae7abc4b72af4afc"},
    {"HUMANTOR.FIF", 22073, "f05f07c0effb3bffcd3bc7036a94bb51"},
    {"IRONMAN.FIF", 24738, "079a7814805437dbd8234a1ae3d5b8a2"},
    {"IRONMAN2.FTT", 1306, "1b0834b018aa549e274e30c16bc24d73"},
    {"MRVLIMAG.ADC", 321753, "1dd4756930561644f00ef9d9aa309af1"},
    {"PUNISHER.FIF", 28273, "3014f8e2e62ca495cc11a1b471563188"},
    {"PUNISHER.FTT", 4122, "2ec32c5275ffeeb547f8d11b3efbd397"},
    {"PUNISHLA.FIF", 5714, "89ce4959260f549049bba4dc5c5c8e9d"},
    {"SPIDEYVE.FIF", 57734, "b7dffec3cf9b5273af3fb27c8c71aed2"},
    {"SPIDEYVE.FTT", 2074, "f11a0ff6b7f8afbeeab72667b050d640"},
    {"SPIDSWIN.FIF", 35645, "1d28dc61f5da9909dc5c0e012f644797"},
    {"THORA.FIF", 23335, "9c084d31bcb5053b8b256415780aaf7b"},
    {"THORA.FTT", 4122, "d7950d62c2de4687086b6d91c5daf158"},
    {"WOLNSAB.FIF", 23954, "82f70f4a9d42eca2c87efb1a4c1f7352"},
    {"WOLVIE.FIF", 21663, "0f04b2a0a3a9baa55ee283619cf9d371"},
    {"WOLVRIP.FIF", 28008, "c9550713cdfbea63e08f510d4153f09b"},
    {"WOMENFX.FIF", 30501, "d560ede8bb6f21cc3b00b3a2c83db96e"},
    {"WOMENFX.FTT", 4122, "80cc36dcdc3c5e6deedef3d32b3fe1f7"},
    {"X-MENVS.FIF", 42179, "49f2849a6cf6758270f04179641477b9"},
    {"XMEN2099.FIF", 44523, "52657c07af2af358a26e4f75be296e6b"},
    {"XMENATTA.FIF", 32823, "71f6e7124db9ffdd7b328c04ea25d052"},
    {"PUNISHLO.FTT", 1306, "fd122f507b088f723e66306b75f1268e"},
    {"SABRETOO.FIF", 20438, "6a2c1b9bba1dec86e2c61b7f209a363c"},
    {"SHE-HULK.FIF", 32874, "415d35eadce33b2c869291f57716d525"},
    {"SHE-HULK.FTT", 4122, "a3f316804538a5ff83b75abcfcafc50c"},
    {"SILVERSA.FIF", 11824, "16d890acf2ddb50a4bf69247a3d89388"},
    {"SILVERSU.FIF", 40387, "10b7ecf41ccac28e14abea438ccb351d"},
    {"SILVERSU.FTT", 2074, "cf27e6b1a6f4d5a88ca1aa9a1366b15a"},
    {"SILVRSU2.FIF", 20805, "8706e0c2b33ceaa3efde596e453dfca0"},
    {"SILVRSU2.FTT", 4122, "dcc26d6f52c05bc581337e20fc663704"},
    {"SPIDATTA.FIF", 34532, "c83360161beff8b57ad5e6393354614b"},
    {"SPIDERCO.FIF", 34100, "e5af1701ace160d8cf7f06f4ddbe1cdc"},
    {"SPIDEY#1.FTT", 4122, "68d6c10f8d48d0157b94386acf4d9b2f"},
    {"SPIDEY1W.FIF", 31291, "4c4416b83cccb0804583542602b9e7ef"},
    {"SPIDEYAT.FTT", 2074, "d1e153a692db6ce489b1dbda70ef02d5"},
    {"SPIDEYID.FIF", 40157, "dc9f0b5812de57c19533644c74baef1d"},
    {"SPIDEYSW.FTT", 2074, "89e9938a59d0343949cb708e10201a8f"},
};
inline constexpr IszRealFile kMarvel1Volumes[] = {
    {"Disk2/MODULES.LIB", 83530, "1895570699390ecf324ad931985dd5dd"},
};
inline constexpr IszRealFile kMarvel1Members[] = {
    {"MARVEL.AD", 81152, "9aa3000e817022c32282c0ea4d260bca"},
    {"DECO.DLL", 137320, "c813d80378a53776ec9b073098883ace"},
};
inline constexpr IszRealFile kMarvel2Volumes[] = {
    {"Disk2/ENGINE.LIB", 165471, "b41904ca27cb2eb1475e87385beccdda"},
};
inline constexpr IszRealFile kMarvel2Members[] = {
    {"AD.EXE", 307200, "766bb9f3b570bcdf358b98f8c0f8b0f3"},
    {"ADINIT.EXE", 1504, "d42b8f287523d6bb23c0651a41aa866d"},
    {"AD_LIB.DLL", 9696, "3bb7d6ebf915ff186add66d104352941"},
    {"MRVLREAD.TXT", 49, "9730ee79a52af11b232300591bed37c9"},
    {"MARVEL.TXT", 1240, "b1bbb36ce47a9c4c7e6efab2eb60ea99"},
    {"AD_MPT.DRV", 4096, "b9004721964e2135fc0c113b080cbf44"},
    {"AD_SB.DRV", 18752, "8fc06c7fe482bf32d671a1a249364a12"},
    {"AD_MME.DRV", 16544, "86c51f7c16218d07ee06187935eb4e08"},
    {"MRVL.WRI", 44800, "8227db59ebb15e6f3ce08567ada919de"},
    {"EDITFILE.TXT", 1264, "c0ee822073e3e48ead09030f5a44a3ef"},
    {"MARVELAD.TXT", 2438, "d5171d22c6adbd7b9b9282423c71b70a"},
};
inline constexpr IszRealFile kMarvel3Volumes[] = {
    {"Disk2/WIN.LIB", 53926, "85a80a761ef02ef72253a3e781ad77fc"},
};
inline constexpr IszRealFile kMarvel3Members[] = {
    {"AD.HLP", 37380, "2173b5a8fac3a1de7d029531b4cff370"},
    {"AD_SND.DLL", 15344, "e92915798f23152b4dc8ee6d602a5d75"},
    {"AD_WRAP.COM", 12272, "c5c899c118c79328dd6e6f2c435e6de6"},
    {"SPALETTE.DLL", 30489, "bcb25a66dfc3e73a4b0b7c5471cdd302"},
    {"AD_PREFS.INI", 1057, "fd22a9e957d8db256fef232958e21390"},
};
inline constexpr IszRealFile kMarvel4Volumes[] = {
    {"Disk2/WINSYS.LIB", 872, "6763d7b544b4abe4ce8c69d30a2e595b"},
};
inline constexpr IszRealFile kMarvel4Members[] = {
    {"AD.386", 5222, "f7ffc8a97b30fc397c2aafa05da9ca66"},
};
inline constexpr IszRealFile kMarvel5Volumes[] = {
    {"Disk1/~INS0762.LIB", 5761, "88dac700bc2e1e88189e5e3e8b06daa9"},
};
inline constexpr IszRealFile kMarvel5Members[] = {
    {"RESOURCE.DLL", 11312, "da13ba3974d946641fcfadf38777c07b"},
};
inline constexpr IszRealFile kSnoopy0Volumes[] = {
    {"Disk1/AD_MODS.1", 1022292, "f177b0cb50d5d61b95f8d8cf06cbb2c2"},
    {"Disk2/AD_MODS.2", 761693, "9fc227b4dc47a4c09ad5fdb114bdae20"},
};
inline constexpr IszRealFile kSnoopy0Members[] = {
    {"IS_COLAG.AD", 523792, "36f4e40f94f24f54533faf6e57c3b847"},
    {"IS_DANCE.AD", 942552, "f31271567b6826507b581609fba1fdab"},
    {"IS_FACES.AD", 340150, "39ad963941eb2c5fc9113866dda449e4"},
    {"IS_FLY.AD", 715707, "d8baf193cd40ab2c32af2cb8355c717d"},
    {"IS_LINUS.AD", 322986, "c7f7e451d574982c1193700c65ad9ede"},
    {"IS_LITRY.AD", 559120, "7b8128dc3bdc1a8f61c9cb455e3619c6"},
    {"IS_SPTLT.AD", 378943, "a62d197cf6b9e8d58be8808def7e4c1b"},
    {"IS_THRPY.AD", 261677, "cb1b272abe48886e620e298ec88278b6"},
};

inline constexpr IszRealZip kIszRealZips[] = {
    {"marvel", "After Dark - Marvel Comics.zip", 2046286, "6981b36abb04779a076466fabad3721c"},
    {"snoopy", "After Dark - Snoopy.zip", 1993700, "a712447e1c957767bdbca884cead02dc"},
};

inline constexpr IszRealLibrary kIszRealLibraries[] = {
    {"6981b36abb04779a076466fabad3721c", "images.lib", kMarvel0Volumes, 2, kMarvel0Members, 60},
    {"6981b36abb04779a076466fabad3721c", "modules.lib", kMarvel1Volumes, 1, kMarvel1Members, 2},
    {"6981b36abb04779a076466fabad3721c", "engine.lib", kMarvel2Volumes, 1, kMarvel2Members, 11},
    {"6981b36abb04779a076466fabad3721c", "win.lib", kMarvel3Volumes, 1, kMarvel3Members, 5},
    {"6981b36abb04779a076466fabad3721c", "winsys.lib", kMarvel4Volumes, 1, kMarvel4Members, 1},
    {"6981b36abb04779a076466fabad3721c", "~INS0762.LIB", kMarvel5Volumes, 1, kMarvel5Members, 1},
    {"a712447e1c957767bdbca884cead02dc", "AD_MODS.z", kSnoopy0Volumes, 2, kSnoopy0Members, 8},
};

}  // namespace test
