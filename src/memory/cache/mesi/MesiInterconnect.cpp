#include "MesiInterconnect.hpp"
#include "MESICache.hpp"

MesiInterconnect::MesiInterconnect(size_t dram_bytes)
: dram_(dram_bytes, 0) {}

void MesiInterconnect::connect(MESICache* c) {
  std::lock_guard<std::recursive_mutex> lk(mtx_);
  caches_.push_back(c);
}

bool MesiInterconnect::any_other_has_line_(int except_id, uint64_t addr) const {
  for (int i = 0; i < (int)caches_.size(); ++i) {
    if (i == except_id) continue;
    auto* cc = caches_[i];
    if (!cc) continue;
    if (cc->hasLine(addr)) return true;
  }
  return false;
}

void MesiInterconnect::snoop_others_(const BusTransaction& t) {
  for (int i = 0; i < (int)caches_.size(); ++i) {
    if (i == t.src_pe) continue;
    if (!caches_[i]) continue;
    caches_[i]->onSnoop(t); 
  }
}

void MesiInterconnect::emit(const BusTransaction& t) { 
  std::lock_guard<std::recursive_mutex> lk(mtx_);
  const uint64_t b = base_(t.addr);

  // A) Capturar Flush (intervención) + write-back a DRAM
  if (t.type == BusMsg::Flush && t.payload && t.size == MESICache::kLineSize) {
    auto& slot = last_flush_[b];
    std::memcpy(slot.data(), t.payload, MESICache::kLineSize);
    std::memcpy(dram_.data()+b, slot.data(), MESICache::kLineSize);
    return;
  }

  // B) Snoop a las demás caches
  snoop_others_(t);

  if (t.type == BusMsg::BusRd || t.type == BusMsg::BusRdX) {
    bool shared = false;
    if (t.type == BusMsg::BusRd) {
      shared = any_other_has_line_(t.src_pe, t.addr);
    }

    std::array<uint8_t, MESICache::kLineSize> line{};
    if (auto it = last_flush_.find(b); it != last_flush_.end()) {
      line = it->second;
      last_flush_.erase(it);
    } else {
      std::memcpy(line.data(), dram_.data()+b, MESICache::kLineSize);
    }

    // onDataResponse directo al solicitante
    auto* src = caches_[t.src_pe];
    if (src) {
      src->onDataResponse(t.addr, line.data(),
                          (t.type == BusMsg::BusRd) ? shared : false);
    }
    return;
  }

}
