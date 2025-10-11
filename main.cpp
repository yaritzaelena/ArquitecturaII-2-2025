#include "SharedMemory.h"
#include <iostream>
#include <iomanip>

// Simula envío de respuesta (callback)
void print_response(MessageP msg) {
    if (msg->type == MessageType::READ_RESP) {
        std::cout << "[TEST] READ_RESP desde " << msg->src
                  << " addr=" << msg->payload.read_resp.address
                  << " size=" << msg->payload.read_resp.size
                  << " status=" << int(msg->payload.read_resp.status) << "\n";

        std::cout << "  Data: ";
        for (auto b : msg->read_resp_data)
            std::cout << std::hex << std::setw(2) << std::setfill('0') << int(b) << " ";
        std::cout << std::dec << "\n";

    } else if (msg->type == MessageType::WRITE_RESP) {
        std::cout << "[TEST] WRITE_RESP desde " << msg->src
                  << " addr=" << msg->payload.write_resp.address
                  << " status=" << int(msg->payload.write_resp.status) << "\n";
    }
}

int main() {
    SharedMemory mem;

    // --- Escribir 32 bytes ---
    MessageP write_msg = std::make_shared<Message>(MessageType::WRITE_MEM, 0, 1, 0);
    write_msg->payload.write_mem.address = 0;
    write_msg->payload.write_mem.size = 32;

    for (int i = 0; i < 32; ++i)
        write_msg->data_write.push_back(static_cast<uint8_t>(i + 1));

    mem.handle_message(write_msg, print_response);

    // --- Leer los mismos 32 bytes ---
    MessageP read_msg = std::make_shared<Message>(MessageType::READ_MEM, 0, 1, 0);
    read_msg->payload.read_mem.address = 0;
    read_msg->payload.read_mem.size = 32;

    mem.handle_message(read_msg, print_response);

    // --- Probar lectura fuera de rango ---
    MessageP bad_read = std::make_shared<Message>(MessageType::READ_MEM, 0, 1, 0);
    bad_read->payload.read_mem.address = 4090; // 4090 + 32 = fuera de rango
    bad_read->payload.read_mem.size = 32;
    mem.handle_message(bad_read, print_response);

    // --- Ver estadísticas ---
    mem.dump_stats();
    return 0;
}
