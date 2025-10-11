#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <vector>
#include <mutex>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <iostream>

// -------------------------
// Tipos de mensajes mínimos
// -------------------------
enum class MessageType {
    READ_MEM,
    WRITE_MEM,
    READ_RESP,
    WRITE_RESP
};

struct Message {
    MessageType type;
    int src;
    int dst;
    int qos;

    struct {
        struct { uint32_t address; uint32_t size; } read_mem;
        struct { uint32_t address; uint32_t size; } write_mem;
        struct { uint32_t address; uint32_t size; uint8_t status; } read_resp;
        struct { uint32_t address; uint8_t status; } write_resp;
    } payload;

    std::vector<uint8_t> read_resp_data;
    std::vector<uint8_t> data_write;

    Message(MessageType t, int d, int s, int q)
        : type(t), src(s), dst(d), qos(q) {}
};

using MessageP = std::shared_ptr<Message>;

// -------------------------
// Clase SharedMemory
// -------------------------
class SharedMemory {
public:
    explicit SharedMemory(size_t cache_line_bytes = 32);
    void handle_message(MessageP msg, std::function<void(MessageP)> send_response);

    // Para depuración
    void dump_stats(std::ostream &os = std::cout);

private:
    void handle_read(MessageP msg, std::function<void(MessageP)> send_response);
    void handle_write(MessageP msg, std::function<void(MessageP)> send_response);

    std::vector<uint8_t> memory;
    std::mutex memory_mutex;

    // Estadísticas
    std::unordered_map<int, uint64_t> reads_by_pe;
    std::unordered_map<int, uint64_t> writes_by_pe;
    std::unordered_map<int, uint64_t> bytes_read_by_pe;
    std::unordered_map<int, uint64_t> bytes_written_by_pe;

    uint64_t total_reads = 0;
    uint64_t total_writes = 0;

    size_t cache_line_size;
};

#endif // SHARED_MEMORY_H
