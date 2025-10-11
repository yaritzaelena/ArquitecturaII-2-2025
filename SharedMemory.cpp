#include "SharedMemory.h"
#include <cstring>
#include <iostream>

static constexpr size_t MEM_WORDS = 512;            // 512 posiciones de 64 bits
static constexpr size_t WORD_BYTES = 8;             // 64 bits = 8 bytes
static constexpr size_t MEM_BYTES = MEM_WORDS * WORD_BYTES; // 512 * 8 = 4096
static constexpr size_t CACHE_LINE_SIZE = 32;       // Tamaño estándar de línea de caché

SharedMemory::SharedMemory() : memory(MEM_BYTES, 0) {}

void SharedMemory::handle_message(MessageP msg, std::function<void(MessageP)> send_response) {
    if (!msg) return;

    switch (msg->type) {
        case MessageType::READ_MEM:
            handle_read(msg, send_response);
            break;
        case MessageType::WRITE_MEM:
            handle_write(msg, send_response);
            break;
        default:
            std::cerr << "[SharedMemory] Tipo de mensaje desconocido\n";
            break;
    }
}

void SharedMemory::handle_read(MessageP msg, std::function<void(MessageP)> send_response) {
    uint32_t addr = msg->payload.read_mem.address;
    uint32_t size = msg->payload.read_mem.size;

    MessageP resp = std::make_shared<Message>(MessageType::READ_RESP, msg->src, -1);
    resp->payload.read_resp.address = addr;
    resp->payload.read_resp.size = size;

    if (size == 0 || addr > MEM_BYTES || size > MEM_BYTES || addr > MEM_BYTES - size) {
        std::cerr << "[SharedMemory] Error: lectura fuera de rango\n";
        resp->payload.read_resp.status = 0x0;
        send_response(resp);
        return;
    }

    std::vector<uint8_t> buffer(size);
    {
        std::lock_guard<std::mutex> lock(memory_mutex);
        std::memcpy(buffer.data(), memory.data() + addr, size);
        total_reads++;
    }

    resp->read_resp_data = std::move(buffer);
    resp->payload.read_resp.status = 0x1;
    send_response(resp);
}

void SharedMemory::handle_write(MessageP msg, std::function<void(MessageP)> send_response) {
    uint32_t addr = msg->payload.write_mem.address;
    uint32_t size = msg->payload.write_mem.size ? msg->payload.write_mem.size : CACHE_LINE_SIZE;
    const auto &data = msg->data_write;

    MessageP resp = std::make_shared<Message>(MessageType::WRITE_RESP, msg->src, -1);
    resp->payload.write_resp.address = addr;

    if (size == 0 || addr > MEM_BYTES || size > MEM_BYTES || addr > MEM_BYTES - size) {
        std::cerr << "[SharedMemory] Error: escritura fuera de rango\n";
        resp->payload.write_resp.status = 0x0;
        send_response(resp);
        return;
    }

    if (data.size() < size) {
        std::cerr << "[SharedMemory] Error: tamaño de datos insuficiente\n";
        resp->payload.write_resp.status = 0x0;
        send_response(resp);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex);
        std::memcpy(memory.data() + addr, data.data(), size);
        total_writes++;
    }

    resp->payload.write_resp.status = 0x1;
    send_response(resp);
}

void SharedMemory::dump_stats(std::ostream &os) {
    std::lock_guard<std::mutex> lock(memory_mutex);
    os << "\n=== Estadísticas de SharedMemory ===\n";
    os << "Total de lecturas: " << total_reads << "\n";
    os << "Total de escrituras: " << total_writes << "\n";
}
