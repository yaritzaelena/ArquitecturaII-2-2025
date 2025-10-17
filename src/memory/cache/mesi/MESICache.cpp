#include "MESICache.hpp"
#include "MesiDebug.hpp"
#include <cstring>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "MesiInterconnect.hpp"   

MESICache::MESICache(int pe_id, MesiInterconnect& bus)
  : pe_id_(pe_id), bus_(&bus) {}

bool MESICache::hasLine(uint64_t addr) const {
  const uint32_t s = idx(addr);
  const uint64_t t = tag(addr);
  for (int w = 0; w < kWays; ++w) {
    const auto& L = sets_[s].way[w];
    if (L.valid && L.tag == t && L.state != MESI::I) {
      return true;
    }
  }
  return false;
}

auto MESICache::lookupLine(uint64_t addr) -> Lookup {
  uint32_t s = idx(addr);
  uint64_t t = tag(addr);
  for (int w=0; w<kWays; ++w) {
    auto& L = sets_[s].way[w];
    if (L.valid && L.tag==t && L.state!=MESI::I) return {true, w, &L};
  }
  return {false, -1, nullptr};
}

void MESICache::touchLRU(uint32_t s, int way_mru) {
  sets_[s].lru = (way_mru==0) ? 1 : 0;
}

int MESICache::victimWay(uint32_t s) const {
  return sets_[s].lru==0 ? 0 : 1;
}

void MESICache::recordTrans(MESI from, MESI to) {
  metrics_.mesi_trans[(int)from][(int)to]++;
  std::stringstream ss;
  ss << "MESI: " 
     << (int)from << "->" << (int)to;
  metrics_.mesi_transitions.push_back(ss.str());
}

void MESICache::emitBusRd(uint64_t addr) {
  metrics_.busRd++;
  assert(bus_);
  bus_->emit({BusMsg::BusRd, addr, nullptr, 0, pe_id_});
}

void MESICache::emitBusRdX(uint64_t addr) {
  metrics_.busRdX++;
  assert(bus_);
  bus_->emit({BusMsg::BusRdX, addr, nullptr, 0, pe_id_});
}

void MESICache::emitBusUpgr(uint64_t addr) {
  metrics_.busUpgr++;
  assert(bus_);
  bus_->emit({BusMsg::BusUpgr, addr, nullptr, 0, pe_id_});
}

void MESICache::emitFlush(uint64_t addr, const uint8_t* data) {
  metrics_.flush++;
  assert(bus_);
  bus_->emit({BusMsg::Flush, addr, data, kLineSize, pe_id_});  // usa kLineSize
}

void MESICache::emitInv(uint64_t addr) {
  assert(bus_);
  bus_->emit({BusMsg::Inv, addr, nullptr, 0, pe_id_});
}


void MESICache::installLine(uint64_t addr, const uint8_t data[32], MESI st) {
  uint32_t s = idx(addr);
  uint64_t t = tag(addr);
  int way = -1;
  for (int w=0; w<kWays; ++w) {
    if (!sets_[s].way[w].valid || sets_[s].way[w].state==MESI::I) { way=w; break; }
  }
  if (way == -1) {
    way = victimWay(s);
    auto& V = sets_[s].way[way];
    if (V.valid && V.state==MESI::M) {
      emitFlush(addr & ~((uint64_t)kLineSize-1), V.data.data());
    }
  }
  auto& L = sets_[s].way[way];
  L.valid = true;
  L.dirty = (st==MESI::M);
  L.state = st;
  L.tag   = t;
  std::memcpy(L.data.data(), data, kLineSize);
  touchLRU(s, way);
}

void MESICache::write8(CacheLine& line, uint32_t line_off, const void* in8) {
  line.dirty = true;
  std::memcpy(line.data.data() + line_off, in8, 8);
}

void MESICache::read8(const CacheLine& line, uint32_t line_off, void* out8) const {
  std::memcpy(out8, line.data.data() + line_off, 8);
}

bool MESICache::load(uint64_t addr, void* out8) {
  metrics_.loads++; metrics_.rw_accesses++;
  auto L = lookupLine(addr);
  uint32_t s = idx(addr);
  uint32_t o = off(addr);

  if (L.hit) {
    if (L.line->state==MESI::I) { /* robustez */ }
    read8(*L.line, o, out8);
    touchLRU(s, L.way);
    return true;
  }

  metrics_.cache_misses++;
  emitBusRd(addr);
  return false;
}

bool MESICache::store(uint64_t addr, const void* in8) {
  metrics_.stores++; metrics_.rw_accesses++;
  auto L = lookupLine(addr);
  uint32_t s = idx(addr);
  uint32_t o = off(addr);

  if (!L.hit || L.line->state==MESI::I) {
    metrics_.cache_misses++;
    emitBusRdX(addr);
    return false;
  }

  switch (L.line->state) {
    case MESI::M:
      write8(*L.line, o, in8); touchLRU(s, L.way); return true;
    case MESI::E:
      recordTrans(MESI::E, MESI::M);
      L.line->state = MESI::M; L.line->dirty = true;
      write8(*L.line, o, in8); touchLRU(s, L.way); return true;
    case MESI::S:
      emitBusUpgr(addr);
      recordTrans(MESI::S, MESI::M);
      L.line->state = MESI::M; L.line->dirty = true;
      write8(*L.line, o, in8); touchLRU(s, L.way); return true;
    case MESI::I:
      break;
  }
  return false;
}

void MESICache::onDataResponse(uint64_t addr, const uint8_t lineData[32], bool shared) {
  installLine(addr, lineData, shared ? MESI::S : MESI::E);
}

void MESICache::onSnoop(const BusTransaction& t) {
  uint32_t s = idx(t.addr);
  uint64_t ttag = tag(t.addr);
  for (int w=0; w<kWays; ++w) {
    auto& L = sets_[s].way[w];
    if (!(L.valid && L.tag==ttag)) continue;

    switch (t.type) {
      case BusMsg::BusRd:
        if (L.state==MESI::M) {
          emitFlush(t.addr, L.data.data());
          recordTrans(MESI::M, MESI::S);
          L.state = MESI::S; L.dirty=false;
        } else if (L.state==MESI::E) {
          recordTrans(MESI::E, MESI::S);
          L.state = MESI::S;
        }
        break;
      case BusMsg::BusRdX:
      case BusMsg::Inv:
        if (L.state==MESI::M) emitFlush(t.addr, L.data.data());
        if (L.state != MESI::I) {
          metrics_.invalidations++;
          recordTrans(L.state, MESI::I);
          L.state = MESI::I; L.dirty=false;
        }
        break;
      case BusMsg::BusUpgr:
        if (L.state==MESI::M) emitFlush(t.addr, L.data.data());
        if (L.state != MESI::I) {
          metrics_.invalidations++;
          recordTrans(L.state, MESI::I);
          L.state = MESI::I; L.dirty=false;
        }
        break;
      default: break;
    }
  }
}

void MESICache::dumpCacheState(std::ostream& os) const {
    os << "=== Estado Cache PE" << pe_id_ << " ===\n";
    for (size_t s = 0; s < kSets; ++s) {
        os << "Set " << s << ":\n";
        for (int w = 0; w < kWays; ++w) {
            const auto& L = sets_[s].way[w];
            os << "  Way " << w << ": ";
            if (!L.valid) { os << "Invalid\n"; continue; }

            const char* st_str = "";
            switch(L.state) {
                case MESI::M: st_str = "M"; break;
                case MESI::E: st_str = "E"; break;
                case MESI::S: st_str = "S"; break;
                case MESI::I: st_str = "I"; break;
            }
            os << st_str 
               << " Tag:0x" << std::hex << L.tag 
               << " Dirty:" << std::dec << L.dirty << "\n";
        }
    }
}

