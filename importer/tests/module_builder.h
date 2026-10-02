// Synthetic After Dark-shaped module files for the catalog tests: a PE32 DLL
// and an NE DLL carrying exactly the resources, imports and exports the
// catalog reads (type 1000 control records, 2000 text, VERSIONINFO,
// STRINGLIST; an Intermission IMX module's SAVERINIT/SAVERDRAW exports),
// built byte by byte. They hold no After Dark bytes — the records are laid
// out from ABI.md §2.10.2 with made-up contents.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace test {

// A little-endian byte buffer addressed by absolute offset.
struct Bytes {
  std::string d;
  void grow(size_t end) {
    if (d.size() < end) d.resize(end, '\0');
  }
  void u8(size_t off, uint8_t v) {
    grow(off + 1);
    d[off] = char(v);
  }
  void u16(size_t off, uint16_t v) {
    u8(off, uint8_t(v));
    u8(off + 1, uint8_t(v >> 8));
  }
  void u32(size_t off, uint32_t v) {
    u16(off, uint16_t(v));
    u16(off + 2, uint16_t(v >> 16));
  }
  void put(size_t off, std::string_view s) {
    grow(off + s.size());
    d.replace(off, s.size(), s);
  }
  size_t align(size_t a) {
    grow((d.size() + a - 1) / a * a);
    return d.size();
  }
};

inline std::string utf16(std::string_view ascii) {
  std::string o;
  for (char c : ascii) {
    o += c;
    o += '\0';
  }
  return o;
}

// ---- control records (ABI.md §2.10.2) --------------------------------------------

inline std::string fixed(std::string_view s, size_t n) {
  std::string o(s.substr(0, n));
  o.resize(n, '\0');
  return o;
}

// The 0x20-byte head every kind shares.
inline std::string record_head(uint16_t kind, std::string_view name, uint16_t count, int16_t dflt) {
  Bytes b;
  b.u16(0, kind);
  b.put(2, fixed(name, 20));
  b.u16(0x16, count);
  b.u16(0x18, uint16_t(dflt));
  b.grow(0x20);
  return b.d;
}

inline std::string string_slider_record(std::string_view name, const std::vector<std::string>& labels,
                                        const std::vector<uint16_t>& values, int16_t dflt) {
  std::string r = record_head(1, name, uint16_t(labels.size()), dflt);
  for (const auto& l : labels) r += fixed(l, 16);
  for (uint16_t v : values) {
    r += char(v & 0xFF);
    r += char(v >> 8);
  }
  return r;
}

inline std::string num_slider_record(std::string_view name, int16_t mn, int16_t mx, int16_t dflt,
                                     std::string_view unit = {}, uint16_t unit_pos = 0) {
  Bytes b;
  b.put(0, record_head(2, name, 0, dflt));
  b.put(0x20, fixed(unit, 16));
  b.u16(0x30, uint16_t(mn));
  b.u16(0x32, uint16_t(mx));
  b.u16(0x34, 0);
  b.u16(0x36, unit_pos);
  return b.d;
}

inline std::string popup_record(std::string_view name, const std::vector<std::string>& items, int16_t dflt) {
  std::string r = record_head(3, name, uint16_t(items.size()), dflt);
  for (const auto& i : items) r += fixed(i, 16);
  return r;
}

inline std::string checkbox_record(std::string_view name, int16_t dflt) { return record_head(5, name, 0, dflt); }
inline std::string button_record(std::string_view name) { return record_head(4, name, 0, 0); }

inline std::string stringlist(const std::vector<std::string>& items) {
  Bytes b;
  b.u16(0, uint16_t(items.size()));
  std::string o = b.d;
  for (const auto& s : items) o += s + '\0';
  return o;
}

// ---- VERSIONINFO (32-bit layout) ------------------------------------------------------

inline std::string version_node(std::string_view key, std::string_view value, uint16_t value_len, uint16_t type,
                                const std::vector<std::string>& children) {
  Bytes b;
  b.u16(2, value_len);
  b.u16(4, type);
  b.put(6, utf16(key) + std::string(2, '\0'));
  b.align(4);
  b.put(b.d.size(), value);
  b.align(4);
  for (size_t i = 0; i < children.size(); i++) {
    b.put(b.d.size(), children[i]);
    if (i + 1 < children.size()) b.align(4);
  }
  b.u16(0, uint16_t(b.d.size()));
  return b.d;
}

inline std::string version_resource(const std::vector<std::pair<std::string, std::string>>& strings) {
  std::vector<std::string> nodes;
  for (const auto& [k, v] : strings)
    nodes.push_back(version_node(k, utf16(v) + std::string(2, '\0'), uint16_t(v.size() + 1), 1, {}));
  std::string table = version_node("040904E4", {}, 0, 1, nodes);
  std::string sfi = version_node("StringFileInfo", {}, 0, 1, {table});
  Bytes fixed_info;
  fixed_info.u32(0, 0xFEEF04BD);
  fixed_info.u32(4, 0x00010000);
  fixed_info.grow(52);
  return version_node("VS_VERSION_INFO", fixed_info.d, 52, 0, {sfi});
}

// ---- PE32 ------------------------------------------------------------------------

struct PeResource {
  uint16_t type_id = 0;        // used when type_name is empty
  std::string type_name;       // "STRINGLIST"
  uint16_t name = 0;
  uint16_t lang = 0x409;
  std::string data;
};

struct PeSpec {
  std::vector<std::string> imports;   // DLL names, one function each
  std::vector<std::string> exports;   // exported names
  std::vector<PeResource> resources;
};

// A DLL with one section (RVA 0x1000) holding the import, export and
// resource directories.
inline std::string build_pe(const PeSpec& spec) {
  const uint32_t kSecRva = 0x1000, kSecFile = 0x200;
  Bytes sec;  // section contents; RVA = kSecRva + offset
  auto rva = [&](size_t off) { return uint32_t(kSecRva + off); };
  sec.grow(16);  // a dummy "function" at the start for the exports to point at

  // Imports: descriptors, then per DLL a lookup table, an IAT, a hint/name.
  uint32_t import_rva = 0, import_size = 0;
  if (!spec.imports.empty()) {
    size_t desc = sec.align(4);
    size_t n = spec.imports.size();
    sec.grow(desc + 20 * (n + 1));
    for (size_t i = 0; i < n; i++) {
      size_t hint = sec.align(2);
      sec.u16(hint, 0);
      sec.put(hint + 2, "fn" + std::to_string(i) + '\0');
      size_t name = sec.align(2);
      sec.put(name, spec.imports[i] + '\0');
      size_t ilt = sec.align(4);
      sec.u32(ilt, rva(hint));
      sec.u32(ilt + 4, 0);
      size_t iat = sec.align(4);
      sec.u32(iat, rva(hint));
      sec.u32(iat + 4, 0);
      size_t d = desc + 20 * i;
      sec.u32(d, rva(ilt));
      sec.u32(d + 12, rva(name));
      sec.u32(d + 16, rva(iat));
    }
    import_rva = rva(desc);
    import_size = uint32_t(20 * (n + 1));
  }

  // Exports: every name on its own function, all pointing at the dummy.
  uint32_t export_rva = 0, export_size = 0;
  if (!spec.exports.empty()) {
    size_t n = spec.exports.size();
    size_t dir = sec.align(4);
    sec.grow(dir + 40);
    size_t funcs = sec.align(4);
    sec.grow(funcs + 4 * n);
    size_t names = sec.align(4);
    sec.grow(names + 4 * n);
    size_t ords = sec.align(4);
    sec.grow(ords + 2 * n);
    size_t dll = sec.d.size();
    sec.put(dll, std::string("SYNTH.AD") + '\0');
    for (size_t i = 0; i < n; i++) {
      size_t s = sec.d.size();
      sec.put(s, spec.exports[i] + '\0');
      sec.u32(funcs + 4 * i, rva(0));
      sec.u32(names + 4 * i, rva(s));
      sec.u16(ords + 2 * i, uint16_t(i));
    }
    size_t end = sec.align(4);
    sec.u32(dir + 12, rva(dll));
    sec.u32(dir + 16, 1);
    sec.u32(dir + 20, uint32_t(n));
    sec.u32(dir + 24, uint32_t(n));
    sec.u32(dir + 28, rva(funcs));
    sec.u32(dir + 32, rva(names));
    sec.u32(dir + 36, rva(ords));
    export_rva = rva(dir);
    export_size = uint32_t(end - dir);
  }

  // Resources: type -> name -> language, named types first as the format
  // requires, each level sorted.
  uint32_t res_rva = 0, res_size = 0;
  if (!spec.resources.empty()) {
    struct TypeKey {
      bool named;
      std::string name;
      uint16_t id;
      bool operator<(const TypeKey& o) const {
        if (named != o.named) return named;
        return named ? name < o.name : id < o.id;
      }
    };
    std::map<TypeKey, std::map<uint16_t, std::map<uint16_t, const PeResource*>>> tree;
    for (const auto& r : spec.resources)
      tree[TypeKey{!r.type_name.empty(), r.type_name, r.type_id}][r.name][r.lang] = &r;
    size_t root = sec.align(4);
    Bytes rs;  // the resource section, offsets relative to root
    auto dir_header = [&](size_t off, uint16_t named, uint16_t ids) {
      rs.grow(off + 16);
      rs.u16(off + 12, named);
      rs.u16(off + 14, ids);
    };
    // Sizes first: every directory, then data entries, then strings and data.
    size_t pos = 16 + 8 * tree.size();
    std::vector<size_t> type_dirs, name_dirs, entries;
    for (auto& [tk, names] : tree) {
      type_dirs.push_back(pos);
      pos += 16 + 8 * names.size();
    }
    for (auto& [tk, names] : tree)
      for (auto& [nm, langs] : names) {
        name_dirs.push_back(pos);
        pos += 16 + 8 * langs.size();
      }
    for (auto& [tk, names] : tree)
      for (auto& [nm, langs] : names)
        for (size_t k = 0; k < langs.size(); k++) {
          entries.push_back(pos);
          pos += 16;
        }
    uint16_t named_types = 0;
    for (auto& [tk, names] : tree) named_types += tk.named;
    dir_header(0, named_types, uint16_t(tree.size() - named_types));
    size_t ti = 0, ni = 0, ei = 0;
    for (auto& [tk, names] : tree) {
      uint32_t id = tk.id;
      if (tk.named) {
        size_t s = std::max(pos, rs.d.size());
        rs.u16(s, uint16_t(tk.name.size()));
        rs.put(s + 2, utf16(tk.name));
        pos = s + 2 + 2 * tk.name.size();
        id = 0x80000000u | uint32_t(s);
      }
      rs.u32(16 + 8 * ti, id);
      rs.u32(16 + 8 * ti + 4, 0x80000000u | uint32_t(type_dirs[ti]));
      dir_header(type_dirs[ti], 0, uint16_t(names.size()));
      size_t k = 0;
      for (auto& [nm, langs] : names) {
        rs.u32(type_dirs[ti] + 16 + 8 * k, nm);
        rs.u32(type_dirs[ti] + 16 + 8 * k + 4, 0x80000000u | uint32_t(name_dirs[ni]));
        dir_header(name_dirs[ni], 0, uint16_t(langs.size()));
        size_t l = 0;
        for (auto& [lang, r] : langs) {
          rs.u32(name_dirs[ni] + 16 + 8 * l, lang);
          rs.u32(name_dirs[ni] + 16 + 8 * l + 4, uint32_t(entries[ei]));
          size_t data = (std::max(pos, rs.d.size()) + 3) & ~size_t(3);
          rs.put(data, r->data);
          pos = data + r->data.size();
          rs.u32(entries[ei], rva(root + data));
          rs.u32(entries[ei] + 4, uint32_t(r->data.size()));
          ei++, l++;
        }
        ni++, k++;
      }
      ti++;
    }
    sec.put(root, rs.d);
    res_rva = rva(root);
    res_size = uint32_t(rs.d.size());
  }
  sec.align(0x200);

  Bytes f;
  f.put(0, "MZ");
  f.u32(0x3C, 0x80);
  f.put(0x80, std::string("PE\0\0", 4));
  size_t coff = 0x84;
  f.u16(coff, 0x014C);
  f.u16(coff + 2, 1);
  f.u16(coff + 16, 0xE0);
  f.u16(coff + 18, 0x2102);  // executable, 32-bit, DLL
  size_t opt = coff + 20;
  uint32_t image = uint32_t(kSecRva + ((sec.d.size() + 0xFFF) & ~size_t(0xFFF)));
  f.u16(opt, 0x10B);
  f.u32(opt + 28, 0x10000000);
  f.u32(opt + 32, 0x1000);
  f.u32(opt + 36, 0x200);
  f.u32(opt + 56, image);
  f.u32(opt + 60, kSecFile);
  f.u16(opt + 68, 2);
  f.u32(opt + 92, 16);
  f.u32(opt + 96 + 8 * 0, export_rva);
  f.u32(opt + 100 + 8 * 0, export_size);
  f.u32(opt + 96 + 8 * 1, import_rva);
  f.u32(opt + 100 + 8 * 1, import_size);
  f.u32(opt + 96 + 8 * 2, res_rva);
  f.u32(opt + 100 + 8 * 2, res_size);
  size_t sh = opt + 0xE0;
  f.put(sh, fixed(".data", 8));
  f.u32(sh + 8, uint32_t(sec.d.size()));
  f.u32(sh + 12, kSecRva);
  f.u32(sh + 16, uint32_t(sec.d.size()));
  f.u32(sh + 20, kSecFile);
  f.u32(sh + 36, 0xC0000040);
  f.put(kSecFile, sec.d);
  return f.d;
}

// ---- NE ----------------------------------------------------------------------------

struct NeResource {
  uint16_t type_id = 0;        // used when type_name is empty
  std::string type_name;
  uint16_t name = 0;
  std::string data;
};

struct NeSpec {
  std::string module_name = "SYNTH";
  std::vector<std::string> module_refs;
  std::vector<NeResource> resources;
  // Exported names: resident names with ordinals 1..n, each an entry of one
  // bundle of fixed entries for segment 1 (the loader does not check entry
  // segments against the segment table; only relocations are validated).
  std::vector<std::string> exports;
  // Names listed after them (ordinals n+1..) with no entry-table entry: a
  // lookup by name finds them, GetProcAddress would not.
  std::vector<std::string> names_without_entries;
  // A program (no library flag), as a Windows 3.1 screen saver is.
  bool program = false;
  // The module description, the non-resident name table's first name
  // ("SCRNSAVE :Made Up"); "" for no table.
  std::string description;
};

// A code-less NE library: header, resource table, name tables, module
// references, the entry table, and the resource data at 16-byte units.
inline std::string build_ne(const NeSpec& spec) {
  const size_t ne = 0x40;
  const uint16_t shift = 4;
  Bytes f;
  f.put(0, "MZ");
  f.u32(0x3C, uint32_t(ne));

  // Resource table, grouped by type in first-seen order.
  std::vector<std::pair<std::pair<std::string, uint16_t>, std::vector<const NeResource*>>> types;
  for (const auto& r : spec.resources) {
    auto key = std::pair(r.type_name, r.type_id);
    auto it = std::find_if(types.begin(), types.end(), [&](auto& t) { return t.first == key; });
    if (it == types.end()) types.push_back({key, {&r}});
    else it->second.push_back(&r);
  }
  Bytes rt;
  rt.u16(0, shift);
  size_t p = 2;
  std::vector<std::pair<size_t, const std::string*>> name_fixups;  // type-id slot -> name
  std::vector<std::pair<size_t, const NeResource*>> data_fixups;    // offset slot -> resource
  for (auto& [key, list] : types) {
    if (key.first.empty()) rt.u16(p, uint16_t(0x8000 | key.second));
    else name_fixups.emplace_back(p, &key.first);
    rt.u16(p + 2, uint16_t(list.size()));
    rt.u32(p + 4, 0);
    p += 8;
    for (const NeResource* r : list) {
      data_fixups.emplace_back(p, r);
      rt.u16(p + 4, 0x0030);
      rt.u16(p + 6, uint16_t(0x8000 | r->name));
      rt.u32(p + 8, 0);
      p += 12;
    }
  }
  rt.u16(p, 0);
  p += 2;
  for (auto& [slot, name] : name_fixups) {
    rt.u16(slot, uint16_t(p));
    rt.u8(p, uint8_t(name->size()));
    rt.put(p + 1, *name);
    p += 1 + name->size();
  }
  rt.u8(p, 0);
  p += 1;

  const size_t rsrc = 0x40;  // relative to the NE header
  size_t resident = rsrc + rt.d.size();
  Bytes rn;
  rn.u8(0, uint8_t(spec.module_name.size()));
  rn.put(1, spec.module_name);
  rn.u16(1 + spec.module_name.size(), 0);
  for (size_t i = 0; i < spec.exports.size(); i++) {
    size_t o = rn.d.size();
    rn.u8(o, uint8_t(spec.exports[i].size()));
    rn.put(o + 1, spec.exports[i]);
    rn.u16(o + 1 + spec.exports[i].size(), uint16_t(i + 1));
  }
  for (size_t i = 0; i < spec.names_without_entries.size(); i++) {
    const std::string& n = spec.names_without_entries[i];
    size_t o = rn.d.size();
    rn.u8(o, uint8_t(n.size()));
    rn.put(o + 1, n);
    rn.u16(o + 1 + n.size(), uint16_t(spec.exports.size() + i + 1));
  }
  rn.u8(rn.d.size(), 0);
  size_t modref = resident + rn.d.size();
  Bytes imp;
  imp.u8(0, 0);  // offset 0 is the conventional empty name
  Bytes mr;
  for (size_t i = 0; i < spec.module_refs.size(); i++) {
    size_t o = imp.d.size();
    imp.u8(o, uint8_t(spec.module_refs[i].size()));
    imp.put(o + 1, spec.module_refs[i]);
    mr.u16(2 * i, uint16_t(o));
  }
  size_t impnames = modref + mr.d.size();
  size_t entry = impnames + imp.d.size();
  // The entry table: one bundle of fixed entries (exported, shared data) in
  // segment 1, then the terminating 0.
  Bytes et;
  if (!spec.exports.empty()) {
    et.u8(0, uint8_t(spec.exports.size()));
    et.u8(1, 1);
    for (size_t i = 0; i < spec.exports.size(); i++) {
      et.u8(2 + 3 * i, 0x03);
      et.u16(3 + 3 * i, uint16_t(16 * i));
    }
  }
  et.u8(et.d.size(), 0);

  f.put(ne, "NE");
  f.u8(ne + 2, 5);
  f.u16(ne + 0x04, uint16_t(entry));
  f.u16(ne + 0x06, uint16_t(std::max<size_t>(et.d.size(), 2)));
  f.u16(ne + 0x0C, spec.program ? 0x0002 : 0x8001);  // a program, multiple data; or a library, single data
  f.u16(ne + 0x1E, uint16_t(spec.module_refs.size()));
  f.u16(ne + 0x22, uint16_t(rsrc));
  f.u16(ne + 0x24, uint16_t(rsrc));
  f.u16(ne + 0x26, uint16_t(resident));
  f.u16(ne + 0x28, uint16_t(modref));
  f.u16(ne + 0x2A, uint16_t(impnames));
  f.u16(ne + 0x32, shift);
  f.u8(ne + 0x36, 2);
  f.u16(ne + 0x3E, 0x030A);
  f.put(ne + resident, rn.d);
  f.put(ne + modref, mr.d);
  f.put(ne + impnames, imp.d);
  f.u16(ne + entry, 0);
  f.put(ne + entry, et.d);

  // Data, each resource at a 16-byte unit, lengths rounded up in units as a
  // resource compiler stores them (so the readers see trailing padding).
  for (auto& [slot, r] : data_fixups) {
    size_t at = f.align(size_t(1) << shift);
    f.put(at, r->data);
    f.align(size_t(1) << shift);
    rt.u16(slot, uint16_t(at >> shift));
    rt.u16(slot + 2, uint16_t((r->data.size() + (1u << shift) - 1) >> shift));
  }
  f.put(ne + rsrc, rt.d);
  if (!spec.description.empty()) {
    // The non-resident name table, at the end of the file: the description
    // (ordinal 0), then the terminating 0.
    Bytes nr;
    nr.u8(0, uint8_t(spec.description.size()));
    nr.put(1, spec.description);
    nr.u16(1 + spec.description.size(), 0);
    nr.u8(nr.d.size(), 0);
    const size_t at = f.d.size();
    f.put(at, nr.d);
    f.u16(ne + 0x20, uint16_t(nr.d.size()));
    f.u32(ne + 0x2C, uint32_t(at));
  }
  return f.d;
}

}  // namespace test
