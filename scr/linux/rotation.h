// The order a rotation (-r, or no module named) plays its modules in: every
// module once per pass, a new pass never starting with the module that just
// ended, and a module named on the command line first (the Windows saver's
// Rotation, scr/src/settings.h). Two orders:
//
//  * ShuffleRotation, a shuffle bag of the player's own, seeded at random,
//    which changes module --cycle seconds after it last did: the player
//    alone (full screen, a window, a preview), and XScreenSaver's players
//    with --different-modules, each in an order of its own;
//  * ClockRotation, the clock's order: XScreenSaver runs one player per
//    monitor (-root), all started at once with the same options, and they
//    are to show the same module and change it together, without a word
//    between them. The time since the Unix epoch is cut into slots of
//    --cycle seconds, and slot n plays the module at position n of one
//    endless order that follows from the modules alone (ClockOrder): every
//    player started with the same modules and --cycle shows the same one,
//    and changes it as its slot ends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace lad {

class Rotation {
 public:
  virtual ~Rotation() = default;
  virtual const std::string& current() const = 0;   // "" when there is nothing to play
  // The rotation moves on (the module changes, or one that kept failing is
  // skipped): the next module.
  virtual const std::string& next() = 0;
  // The modules it plays, a named one that is not among them included.
  virtual size_t size() const = 0;
  bool empty() const { return size() == 0; }
};

// The shuffle bag. `first` plays before the bag: moved to its front when it
// is in it, and otherwise once ahead of it.
class ShuffleRotation : public Rotation {
 public:
  ShuffleRotation(std::vector<std::string> ids, uint32_t seed, const std::string& first = {});
  const std::string& current() const override;
  const std::string& next() override;
  size_t size() const override { return order_.size() + (lead_.empty() ? 0 : 1); }

 private:
  std::vector<std::string> order_;
  std::string lead_;
  size_t pos_ = 0;
  std::mt19937 rng_;
};

// The clock's endless order of N modules: positions pN to pN + N - 1 are
// pass p, which holds every module once and never starts with the module
// that ended pass p - 1 (two modules alternate). It follows from the set of
// ids alone, whatever their order or repeats, through a shuffle of its own
// (splitmix64 and Fisher-Yates; the standard library's shuffle and
// distributions are each library's own), so every player has the same one.
class ClockOrder {
 public:
  explicit ClockOrder(std::vector<std::string> ids);
  size_t size() const { return ids_.size(); }
  const std::string& at(int64_t n) const;   // "" when there are no modules
  bool has(const std::string& id) const;

 private:
  // Pass p's shuffle, as indices into ids_; *rest: its random stream after it.
  std::vector<uint32_t> shuffled(int64_t pass, uint64_t* rest) const;

  std::vector<std::string> ids_;   // sorted, without repeats
  uint64_t key_ = 0;               // a hash of them
};

// The clock's slots, in milliseconds since the Unix epoch: slot n runs from
// n * slot_ms to (n + 1) * slot_ms.
int64_t clock_slot(int64_t unix_ms, int64_t slot_ms);
int64_t slot_start(int64_t slot, int64_t slot_ms);
// A player's first module plays at least this long (half a slot when that
// is shorter), so none shows for a moment only: a player started nearer the
// end of its slot plays the next slot's module at once, and on to the end
// of that slot. Two players started either side of that point, a moment
// apart, differ for those seconds at most.
inline constexpr int64_t kFirstSlotMs = 10000;
int64_t first_slot(int64_t unix_ms, int64_t slot_ms);

// The clock's order as it plays, from a first slot on. The module named on
// the command line (`first`) plays until the first slot ends, and the
// order then takes over at the next slot's position. A module skipped after
// failing moves the order on ahead of the clock: the next position plays
// for the rest of the slot, and on, since the clock comes to it next.
class ClockRotation : public Rotation {
 public:
  ClockRotation(std::vector<std::string> ids, int64_t slot, const std::string& first = {});
  const std::string& current() const override;
  const std::string& next() override;
  size_t size() const override { return order_.size() + (lead_counts_ ? 1 : 0); }
  // The clock is in `slot` now (its previous slot has ended): that slot's
  // module, unless `keep_ahead` and a skip put the order at a later
  // position, whose module then plays on until the clock catches up with
  // it (the caller passes it while that module plays well). A slot earlier
  // than the last one (the wall clock was set back) is followed as it is.
  const std::string& go_to(int64_t slot, bool keep_ahead);
  int64_t slot() const { return slot_; }

 private:
  ClockOrder order_;
  std::string lead_;
  bool leading_ = false;       // the lead plays
  bool lead_counts_ = false;   // it is not among the ids
  int64_t slot_ = 0;           // the slot playing: the next change is at its end
  int64_t pos_ = 0;            // the order's position playing (while the lead plays: the next slot's)
};

}  // namespace lad
