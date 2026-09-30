// The host-owned descriptor table of the Win16 machine (DESIGN.md §2): the
// CPU asks it for every selector a guest loads (cpu::DescriptorProvider).
//
// Selectors are LDT selectors with RPL 3 — (index << 3) | 7 — as every Win16
// selector under Windows 3.1/95 enhanced mode is. A handle to a moveable
// global block is its selector with bit 0 clear (…6), which names the same
// descriptor with RPL 2, so code that loads a handle straight into a segment
// register still works, as it did on Windows. Lookups ignore the RPL bits.
//
// Huge blocks (> 64 KiB) are tiled over consecutive selectors: tile i is
// selector first + i * __AHINCR (8) and is based i * 64 KiB into the block
// (__AHSHIFT 3). Each tile's limit runs to the end of the block, as Windows
// set it, so 386 code that reaches past 64 KiB through the first selector
// with 32-bit offsets keeps working.
//
// Besides the LDT, one GDT selector is known: 0x40, the BIOS data area
// (KERNEL.__0040H), which Borland start-up code writes to.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "X86Emulator.hh"

namespace adw::win16 {

using cpu::SegDesc;

class Ldt : public cpu::DescriptorProvider {
 public:
  static constexpr uint16_t kEntries = 8192;
  static constexpr uint16_t kAhShift = 3;
  static constexpr uint16_t kAhIncr = 1 << kAhShift;  // 8: selector distance between the tiles of a huge block
  static constexpr uint16_t kBiosSelector = 0x0040;

  static constexpr uint16_t selector_of(uint16_t index) { return uint16_t((index << 3) | 7); }
  static constexpr uint16_t index_of(uint16_t sel) { return uint16_t(sel >> 3); }
  static constexpr bool is_ldt(uint16_t sel) { return (sel & 4) != 0; }
  // Tiles a block of `size` bytes needs.
  static uint16_t tiles_for(uint32_t size) { return uint16_t(size ? (size + 0xFFFF) >> 16 : 1); }

  Ldt();

  // `n` consecutive free selectors (first fit from the lowest index, so the
  // same request sequence gives the same selectors); 0 when full.
  uint16_t alloc(uint16_t n = 1);
  // Grows the run [first, first+old_n) to new_n selectors in place if the ones
  // after it are free.
  bool extend(uint16_t first, uint16_t old_n, uint16_t new_n);
  void free(uint16_t first, uint16_t n = 1);
  bool in_use(uint16_t sel) const;

  // Descriptor of one selector.
  void set(uint16_t sel, const SegDesc& d);
  // Descriptors of a (possibly tiled) block: n = tiles_for(size) selectors from `first`.
  void set_block(uint16_t first, uint32_t base, uint32_t size, bool code, bool writable_or_readable = true);
  const SegDesc* get(uint16_t sel) const;
  uint32_t base_of(uint16_t sel) const;   // 0 for unknown selectors
  uint32_t limit_of(uint16_t sel) const;  // 0 for unknown selectors

  // The one GDT selector (0x40).
  void set_bios_area(uint32_t base, uint32_t limit);

  // DescriptorProvider.
  bool lookup(uint16_t sel, SegDesc& out) override;

  // Free-form tag per selector for diagnostics ("TOASTPRO seg 2", "global 0x3C").
  void set_tag(uint16_t sel, std::string tag);
  const std::string& tag(uint16_t sel) const;

  // Runs of selectors KERNEL's selector calls made for the guest
  // (AllocSelector, AllocCStoDSAlias, AllocDStoCSAlias: kernel16.cc): the
  // first of a run is marked with its length, its other tiles with the first.
  // FreeSelector frees whole marked runs and SetSelectorBase/Limit change
  // marked selectors; nothing else is theirs to touch. Freeing or
  // reallocating an entry clears its mark, so a selector reused for anything
  // else is never taken for one of theirs.
  void mark_guest_run(uint16_t first, uint16_t n);
  uint16_t guest_run(uint16_t sel) const;   // the run's length when sel is its first selector, else 0
  bool in_guest_run(uint16_t sel) const;    // sel is one of a marked run's selectors
  uint16_t free_guest_run(uint16_t first);  // frees the run `first` begins; its length, 0 when none

  size_t used() const { return used_; }

 private:
  struct Entry {
    bool used = false;
    SegDesc desc;
    std::string tag;
    uint16_t run = 0;  // mark_guest_run: the length (first selector), or kRunTile | the first's index
  };
  static constexpr uint16_t kRunTile = 0x8000;
  std::vector<Entry> entries_;
  SegDesc bios_;
  bool have_bios_ = false;
  size_t used_ = 0;
};

}  // namespace adw::win16
