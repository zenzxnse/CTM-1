#include "context_hmi/bounded_cache.hpp"

#include <stdexcept>
#include <utility>

namespace context_hmi::cache {

BoundedJsonCache::BoundedJsonCache(Config config) : config_(config) {
  if (config_.capacity == 0 || config_.capacity > 4096 ||
      config_.time_to_live < std::chrono::seconds(1) ||
      config_.time_to_live > std::chrono::hours(24)) {
    throw std::invalid_argument("cache limits are out of range");
  }
  entries_.reserve(config_.capacity);
}

std::optional<Json> BoundedJsonCache::get(const std::string& key) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    ++misses_;
    return std::nullopt;
  }
  if (found->second.expires_at <= now) {
    erase_locked(found);
    ++expirations_;
    ++misses_;
    return std::nullopt;
  }
  recency_.splice(recency_.begin(), recency_, found->second.recency);
  ++hits_;
  return found->second.value;
}

void BoundedJsonCache::put(std::string key, Json value) {
  const auto expiration = Clock::now() + config_.time_to_live;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    existing->second.value = std::move(value);
    existing->second.expires_at = expiration;
    recency_.splice(recency_.begin(), recency_, existing->second.recency);
    return;
  }
  if (entries_.size() >= config_.capacity) {
    const auto oldest = entries_.find(recency_.back());
    if (oldest != entries_.end()) {
      erase_locked(oldest);
      ++evictions_;
    }
  }
  recency_.push_front(std::move(key));
  entries_.emplace(recency_.front(),
                   Entry{std::move(value), expiration, recency_.begin()});
}

void BoundedJsonCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  invalidations_ += entries_.size();
  entries_.clear();
  recency_.clear();
}

Json BoundedJsonCache::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Json{{"entries", entries_.size()},
              {"capacity", config_.capacity},
              {"ttl_ms", config_.time_to_live.count()},
              {"hits", hits_},
              {"misses", misses_},
              {"expirations", expirations_},
              {"evictions", evictions_},
              {"invalidations", invalidations_}};
}

void BoundedJsonCache::erase_locked(
    std::unordered_map<std::string, Entry>::iterator location) {
  recency_.erase(location->second.recency);
  entries_.erase(location);
}

}  /* namespace context_hmi::cache */
