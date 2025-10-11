#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <vector>
#include <mutex>
#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
    int src;   // Identificador del procesador emisor
    int dst;   // Destino del mensaje

    struct {
        struct { uint32_t address; uint32_t size; } read_mem;
        struct { uint32_t address; uint32_t size; } write_mem;
        struct { uint32_t address; uint32_t size; uint8_t status; } read_resp;
        struct { uint32_t address; uint8_t status; } write_resp;
    } payload;

    std::vector<uint8_t> read_resp_data;  // Datos leídos
    std::vector<uint8_t> data_write;      // Datos a escribir

    Message(MessageType t, int d, int s)
        : type(t), src(s), dst(d) {}
};

using MessageP = std::shared_ptr<Message>;

// -------------------------
// Clase SharedMemory
// -------------------------
class SharedMemory {
public:
    SharedMemory();
    void handle_message(MessageP msg, std::function<void(MessageP)> send_response);
    void dump_stats(std::ostream &os = std::cout);

private:
    void handle_read(MessageP msg, std::function<void(MessageP)> send_response);
    void handle_write(MessageP msg, std::function<void(MessageP)> send_response);

    std::vector<uint8_t> memory;
    std::mutex memory_mutex;

    // Estadísticas básicas
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
};

#endif // SHARED_MEMORY_H
