#include "win16/ldt.hh"

#include <algorithm>

namespace adw::win16 {

namespace {
const std::string kNoTag;
}

Ldt::Ldt() : entries_(kEntries) {
  // Index 0 would be selector 0x0007, which is the null selector with TI set:
  // a real LDT has an entry there, but Win16 code treats a zero selector part
  // as NULL, so never hand it out.
  entries_[0].used = true;
}

uint16_t Ldt::alloc(uint16_t n) {
  if (!n) n = 1;
  uint32_t run = 0;
  for (uint32_t i = 1; i < kEntries; i++) {
    run = entries_[i].used ? 0 : run + 1;
    if (run == n) {
      uint32_t first = i + 1 - n;
      for (uint32_t j = first; j <= i; j++) {
        entries_[j] = Entry{};
        entries_[j].used = true;
        // Until set() says otherwise: a present, empty data segment.
        entries_[j].desc = SegDesc{0, 0, true, false, true, false, 3};
      }
      used_ += n;
      return selector_of(uint16_t(first));
    }
  }
  return 0;
}

bool Ldt::extend(uint16_t first, uint16_t old_n, uint16_t new_n) {
  if (new_n <= old_n) return true;
  uint32_t idx = index_of(first);
  if (idx + new_n > kEntries) return false;
  for (uint32_t j = idx + old_n; j < idx + new_n; j++) {
    if (entries_[j].used) return false;
  }
  for (uint32_t j = idx + old_n; j < idx + new_n; j++) {
    entries_[j] = Entry{};
    entries_[j].used = true;
    entries_[j].desc = SegDesc{0, 0, true, false, true, false, 3};
  }
  used_ += new_n - old_n;
  return true;
}

void Ldt::free(uint16_t first, uint16_t n) {
  uint32_t idx = index_of(first);
  for (uint32_t j = idx; j < idx + n && j < kEntries; j++) {
    if (j == 0 || !entries_[j].used) continue;
    entries_[j] = Entry{};
    used_--;
  }
}

bool Ldt::in_use(uint16_t sel) const {
  if (!is_ldt(sel)) return false;
  uint32_t idx = index_of(sel);
  return idx && idx < kEntries && entries_[idx].used;
}

void Ldt::set(uint16_t sel, const SegDesc& d) {
  uint32_t idx = index_of(sel);
  if (!is_ldt(sel) || !idx || idx >= kEntries) return;
  entries_[idx].desc = d;
}

void Ldt::set_block(uint16_t first, uint32_t base, uint32_t size, bool code, bool rw) {
  uint16_t n = tiles_for(size);
  for (uint16_t i = 0; i < n; i++) {
    uint32_t off = uint32_t(i) << 16;
    uint32_t remaining = size > off ? size - off : 0;
    SegDesc d;
    d.base = base + off;
    d.limit = remaining ? remaining - 1 : 0;
    d.present = true;
    d.code = code;
    d.readable_or_writable = rw;
    d.big = false;
    d.dpl = 3;
    set(uint16_t(first + i * kAhIncr), d);
  }
}

const SegDesc* Ldt::get(uint16_t sel) const {
  if (!is_ldt(sel)) {
    if ((sel & ~3) == kBiosSelector && have_bios_) return &bios_;
    return nullptr;
  }
  uint32_t idx = index_of(sel);
  if (!idx || idx >= kEntries || !entries_[idx].used) return nullptr;
  return &entries_[idx].desc;
}

uint32_t Ldt::base_of(uint16_t sel) const {
  const SegDesc* d = get(sel);
  return d ? d->base : 0;
}

uint32_t Ldt::limit_of(uint16_t sel) const {
  const SegDesc* d = get(sel);
  return d ? d->limit : 0;
}

void Ldt::set_bios_area(uint32_t base, uint32_t limit) {
  bios_ = SegDesc{base, limit, true, false, true, false, 3};
  have_bios_ = true;
}

bool Ldt::lookup(uint16_t sel, SegDesc& out) {
  const SegDesc* d = get(sel);
  if (!d) return false;
  out = *d;
  return true;
}

void Ldt::set_tag(uint16_t sel, std::string tag) {
  uint32_t idx = index_of(sel);
  if (!is_ldt(sel) || !idx || idx >= kEntries) return;
  entries_[idx].tag = std::move(tag);
}

const std::string& Ldt::tag(uint16_t sel) const {
  uint32_t idx = index_of(sel);
  if (!is_ldt(sel) || !idx || idx >= kEntries) return kNoTag;
  return entries_[idx].tag;
}

void Ldt::mark_guest_run(uint16_t first, uint16_t n) {
  uint32_t idx = index_of(first);
  if (!is_ldt(first) || !idx || !n || n >= kRunTile || idx + n > kEntries) return;
  for (uint32_t j = idx; j < idx + n; j++) {
    if (!entries_[j].used) return;
  }
  entries_[idx].run = n;
  for (uint32_t j = idx + 1; j < idx + n; j++) entries_[j].run = uint16_t(kRunTile | idx);
}

uint16_t Ldt::guest_run(uint16_t sel) const {
  uint32_t idx = index_of(sel);
  if (!is_ldt(sel) || !idx || idx >= kEntries || !entries_[idx].used) return 0;
  return (entries_[idx].run & kRunTile) ? 0 : entries_[idx].run;
}

bool Ldt::in_guest_run(uint16_t sel) const {
  uint32_t idx = index_of(sel);
  return is_ldt(sel) && idx && idx < kEntries && entries_[idx].used && entries_[idx].run;
}

uint16_t Ldt::free_guest_run(uint16_t first) {
  uint16_t n = guest_run(first);
  if (!n) return 0;
  uint32_t idx = index_of(first);
  free(first, 1);
  // Its tiles, unless one was freed and handed out again meanwhile.
  for (uint32_t j = idx + 1; j < idx + n; j++) {
    if (entries_[j].used && entries_[j].run == uint16_t(kRunTile | idx)) free(selector_of(uint16_t(j)), 1);
  }
  return n;
}

}  // namespace adw::win16
