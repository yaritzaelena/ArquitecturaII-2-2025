#include "MesiInterconnect.hpp"
#include "../src/memory/SharedMemory.h" 
#include "../src/memory/cache/mesi/MESICache.hpp"  
#include "../src/utils/Stepper.hpp"     

#include <memory>
#include <cstring>
#include <algorithm>
#include <cassert>

static_assert(MESICache::kLineSize == 32, "Se espera línea de 32 bytes");

MesiInterconnect::MesiInterconnect(size_t ) {
 
}

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

// --- Helpers de acceso a memoria compartida (línea completa de 32B) ---
void MesiInterconnect::read_line_from_mem_(uint64_t b, uint8_t* out) {
  assert(shm_ && "SharedMemory no adjunta: llama set_shared_memory(&shm) antes de usar el bus");

  auto req = std::make_shared<Message>(MessageType::READ_MEM, /*dst*/-1, /*src*/-1);
  req->payload.read_mem.address = static_cast<uint32_t>(b);
  req->payload.read_mem.size    = MESICache::kLineSize;

  std::vector<uint8_t> data;
  shm_->handle_message(req, [&](MessageP resp){
    if (resp && resp->type == MessageType::READ_RESP && resp->payload.read_resp.status) {
      data = resp->read_resp_data;
    }
  });

  if (data.size() == MESICache::kLineSize) {
    std::memcpy(out, data.data(), MESICache::kLineSize);
  } else {
    // Seguridad: si algo falla, devuelve ceros (evita leer basura)
    std::memset(out, 0, MESICache::kLineSize);
  }
}

void MesiInterconnect::write_line_to_mem_(uint64_t b, const uint8_t* in) {
  assert(shm_ && "SharedMemory no adjunta: llama set_shared_memory(&shm) antes de usar el bus");

  auto req = std::make_shared<Message>(MessageType::WRITE_MEM, /*dst*/-1, /*src*/-1);
  req->payload.write_mem.address = static_cast<uint32_t>(b);
  req->payload.write_mem.size    = MESICache::kLineSize;
  req->data_write.assign(in, in + MESICache::kLineSize);

  // SharedMemory es síncrona
  shm_->handle_message(req, [&](MessageP /*resp*/) {});
}

// --- Camino principal del bus ---
void MesiInterconnect::emit(const BusTransaction& t) {
  std::lock_guard<std::recursive_mutex> lk(mtx_);
  const uint64_t b = base_(t.addr);

  // A) Intervención/Flush: cache en M escribe la línea de vuelta
  if (t.type == BusMsg::Flush && t.payload && t.size == MESICache::kLineSize) {
    auto& slot = last_flush_[b];
    std::memcpy(slot.data(), t.payload, MESICache::kLineSize);

    // Persistir al backing store real (SharedMemory)
    write_line_to_mem_(b, slot.data());

   
    if (stepper_) stepper_->pause("Flush", caches_, shm_);
    return;
  }

  // B) Snoop a las demás cachés (invalidaciones/observaciones)
  snoop_others_(t);


  if (t.type == BusMsg::Inv || t.type == BusMsg::BusUpgr) {
    if (stepper_) stepper_->pause(
      (t.type == BusMsg::Inv) ? "Inv" : "BusUpgr", caches_, shm_);
  }

  // C) Lecturas
  if (t.type == BusMsg::BusRd || t.type == BusMsg::BusRdX) {
    bool shared = false;
    if (t.type == BusMsg::BusRd) {
      shared = any_other_has_line_(t.src_pe, t.addr);
    }

    std::array<uint8_t, MESICache::kLineSize> line{};

    // 1) Si alguien flusheó justo antes, úsalo (ya quedó persistido también)
    if (auto it = last_flush_.find(b); it != last_flush_.end()) {
      line = it->second;
      last_flush_.erase(it);
    } else {
      // 2) Leer línea desde SharedMemory
      read_line_from_mem_(b, line.data());
    }


    if (stepper_) stepper_->pause(
      (t.type == BusMsg::BusRd) ? "BusRd" : "BusRdX", caches_, shm_);

    // D) Responder al solicitante
    auto* src = (t.src_pe >= 0 && t.src_pe < (int)caches_.size()) ? caches_[t.src_pe] : nullptr;
    if (src) {
      src->onDataResponse(t.addr, line.data(),
                          (t.type == BusMsg::BusRd) ? shared : false);
    }
    return;
  }
}
