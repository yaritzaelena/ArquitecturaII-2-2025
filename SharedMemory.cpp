#include "SharedMemory.h"
#include <cstring>

static constexpr size_t MEM_WORDS = 512;            // 512 posiciones de 64 bits
static constexpr size_t WORD_BYTES = 8;             // 64 bits = 8 bytes
static constexpr size_t MEM_BYTES = MEM_WORDS * WORD_BYTES; // 512 * 8 = 4096
static constexpr size_t DEFAULT_CACHE_LINE = 32;

SharedMemory::SharedMemory(size_t cache_line_bytes)
    : memory(MEM_BYTES, 0),
      cache_line_size(cache_line_bytes ? cache_line_bytes : DEFAULT_CACHE_LINE)
{}

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

    MessageP resp = std::make_shared<Message>(MessageType::READ_RESP, -1, msg->src, msg->qos);
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
        reads_by_pe[msg->src]++;
        bytes_read_by_pe[msg->src] += size;
        total_reads++;
    }

    resp->read_resp_data = std::move(buffer);
    resp->payload.read_resp.status = 0x1;
    send_response(resp);
}

void SharedMemory::handle_write(MessageP msg, std::function<void(MessageP)> send_response) {
    uint32_t addr = msg->payload.write_mem.address;
    uint32_t size = msg->payload.write_mem.size ? msg->payload.write_mem.size : cache_line_size;
    const auto &data = msg->data_write;

    MessageP resp = std::make_shared<Message>(MessageType::WRITE_RESP, -1, msg->src, msg->qos);
    resp->payload.write_resp.address = addr;

    if (size == 0 || addr > MEM_BYTES || size > MEM_BYTES || addr > MEM_BYTES - size) {
        std::cerr << "[SharedMemory] Error: escritura fuera de rango\n";
        resp->payload.write_resp.status = 0x0;
        send_response(resp);
        return;
    }

    if (data.size() < size) {
        std::cerr << "[SharedMemory] Error: data_write menor que tamaño esperado\n";
        resp->payload.write_resp.status = 0x0;
        send_response(resp);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex);
        std::memcpy(memory.data() + addr, data.data(), size);
        writes_by_pe[msg->src]++;
        bytes_written_by_pe[msg->src] += size;
        total_writes++;
    }

    resp->payload.write_resp.status = 0x1;
    send_response(resp);
}

void SharedMemory::dump_stats(std::ostream &os) {
    std::lock_guard<std::mutex> lock(memory_mutex);
    os << "\n=== SharedMemory Stats ===\n";
    os << "Total reads: " << total_reads << ", Total writes: " << total_writes << "\n";
    for (auto &kv : reads_by_pe) {
        int pe = kv.first;
        os << "PE " << pe << ": reads=" << kv.second
           << ", bytes=" << bytes_read_by_pe[pe]
           << ", writes=" << writes_by_pe[pe]
           << ", bytes_written=" << bytes_written_by_pe[pe] << "\n";
    }
}
