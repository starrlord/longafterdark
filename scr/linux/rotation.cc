#include "rotation.h"

#include <algorithm>
#include <utility>

namespace lad {

namespace {

// splitmix64's mix, and its stream: well-mixed numbers from any seed, the
// same on every machine and build.
uint64_t mix(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

uint64_t next_random(uint64_t& state) { return mix(state += 0x9E3779B97F4A7C15ull); }

// Division rounded down, for times before the epoch too.
int64_t floor_div(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return a % b != 0 && (a < 0) != (b < 0) ? q - 1 : q;
}

}  // namespace

// ---- ShuffleRotation ---------------------------------------------------------------

ShuffleRotation::ShuffleRotation(std::vector<std::string> ids, uint32_t seed, const std::string& first) : rng_(seed) {
  for (auto& id : ids) {
    if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(std::move(id));
  }
  std::shuffle(order_.begin(), order_.end(), rng_);
  if (first.empty()) return;
  if (auto it = std::find(order_.begin(), order_.end(), first); it != order_.end()) std::iter_swap(order_.begin(), it);
  else lead_ = first;
}

const std::string& ShuffleRotation::current() const {
  static const std::string none;
  if (!lead_.empty()) return lead_;
  return order_.empty() ? none : order_[pos_];
}

const std::string& ShuffleRotation::next() {
  if (!lead_.empty() && !order_.empty()) {
    lead_.clear();
    return order_[pos_];
  }
  if (order_.size() <= 1) return current();
  if (++pos_ < order_.size()) return order_[pos_];
  std::string last = order_.back();
  std::shuffle(order_.begin(), order_.end(), rng_);
  if (order_[0] == last) {
    std::uniform_int_distribution<size_t> d(1, order_.size() - 1);
    std::swap(order_[0], order_[d(rng_)]);
  }
  pos_ = 0;
  return order_[0];
}

// ---- ClockOrder --------------------------------------------------------------------

ClockOrder::ClockOrder(std::vector<std::string> ids) : ids_(std::move(ids)) {
  std::sort(ids_.begin(), ids_.end());
  ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
  // FNV-1a over the ids, each ended by a 0 byte.
  key_ = 0xCBF29CE484222325ull;
  for (const std::string& id : ids_) {
    for (const char c : id) key_ = (key_ ^ (unsigned char)c) * 0x100000001B3ull;
    key_ *= 0x100000001B3ull;
  }
}

bool ClockOrder::has(const std::string& id) const { return std::binary_search(ids_.begin(), ids_.end(), id); }

std::vector<uint32_t> ClockOrder::shuffled(int64_t pass, uint64_t* rest) const {
  std::vector<uint32_t> v(ids_.size());
  for (size_t i = 0; i < v.size(); ++i) v[i] = (uint32_t)i;
  uint64_t s = mix(key_ ^ mix((uint64_t)pass + 0x9E3779B97F4A7C15ull));
  for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[next_random(s) % i]);
  if (rest) *rest = s;
  return v;
}

const std::string& ClockOrder::at(int64_t n) const {
  static const std::string none;
  const int64_t count = (int64_t)ids_.size();
  if (count == 0) return none;
  if (count == 1) return ids_[0];
  const int64_t pass = floor_div(n, count);
  const size_t i = (size_t)(n - pass * count);
  // Two modules alternate (a pass may not start with the one that ended the
  // pass before): every pass is pass 0.
  if (count == 2) return ids_[shuffled(0, nullptr)[i]];
  uint64_t s = 0;
  std::vector<uint32_t> v = shuffled(pass, &s);
  // A pass that would start with the module ending the pass before starts
  // with another, one of its middle ones, never its last: which module ends
  // a pass follows from that pass's shuffle alone.
  if (v[0] == shuffled(pass - 1, nullptr)[(size_t)count - 1]) {
    std::swap(v[0], v[1 + next_random(s) % (uint64_t)(count - 2)]);
  }
  return ids_[v[i]];
}

// ---- the clock's slots -------------------------------------------------------------

int64_t clock_slot(int64_t unix_ms, int64_t slot_ms) { return floor_div(unix_ms, slot_ms); }

int64_t slot_start(int64_t slot, int64_t slot_ms) { return slot * slot_ms; }

int64_t first_slot(int64_t unix_ms, int64_t slot_ms) {
  const int64_t slot = clock_slot(unix_ms, slot_ms);
  const int64_t left = slot_start(slot + 1, slot_ms) - unix_ms;
  return left < std::min(kFirstSlotMs, slot_ms / 2) ? slot + 1 : slot;
}

// ---- ClockRotation -----------------------------------------------------------------

ClockRotation::ClockRotation(std::vector<std::string> ids, int64_t slot, const std::string& first)
    : order_(std::move(ids)), lead_(first), slot_(slot), pos_(slot) {
  if (lead_.empty()) return;
  leading_ = true;
  lead_counts_ = !order_.has(lead_);
  pos_ = slot + 1;
}

const std::string& ClockRotation::current() const { return leading_ ? lead_ : order_.at(pos_); }

const std::string& ClockRotation::next() {
  if (order_.size() == 0) return current();   // the lead alone
  if (!leading_) {
    if (order_.size() > 1) ++pos_;
    return current();
  }
  // The lead gives way to the next slot's module, or the one after it when
  // that is the lead itself (it would only fail again).
  leading_ = false;
  if (order_.size() > 1 && order_.at(pos_) == lead_) ++pos_;
  return current();
}

const std::string& ClockRotation::go_to(int64_t slot, bool keep_ahead) {
  if (order_.size() > 0) leading_ = false;
  if (slot < slot_ || !keep_ahead || pos_ < slot) pos_ = slot;
  slot_ = slot;
  return current();
}

}  // namespace lad
