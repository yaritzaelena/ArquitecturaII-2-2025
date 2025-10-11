#include "SharedMemory.h"
#include <iostream>
#include <iomanip>

// Función de respuesta simulada
void print_response(MessageP msg) {
    if (msg->type == MessageType::READ_RESP) {
        std::cout << "[RESP] READ addr=" << msg->payload.read_resp.address
                  << " size=" << msg->payload.read_resp.size
                  << " status=" << int(msg->payload.read_resp.status) << "\n";
        std::cout << "  Data: ";
        for (auto b : msg->read_resp_data)
            std::cout << std::hex << std::setw(2) << std::setfill('0') << int(b) << " ";
        std::cout << std::dec << "\n";
    } else if (msg->type == MessageType::WRITE_RESP) {
        std::cout << "[RESP] WRITE addr=" << msg->payload.write_resp.address
                  << " status=" << int(msg->payload.write_resp.status) << "\n";
    }
}

int main() {
    SharedMemory mem;

    // Escritura de 32 bytes en dirección 0
    MessageP write_msg = std::make_shared<Message>(MessageType::WRITE_MEM, -1, 1);
    write_msg->payload.write_mem.address = 0;
    write_msg->payload.write_mem.size = 32;
    for (int i = 0; i < 32; ++i)
        write_msg->data_write.push_back(uint8_t(i + 1));

    mem.handle_message(write_msg, print_response);

    // Lectura de los mismos 32 bytes
    MessageP read_msg = std::make_shared<Message>(MessageType::READ_MEM, -1, 1);
    read_msg->payload.read_mem.address = 0;
    read_msg->payload.read_mem.size = 32;
    mem.handle_message(read_msg, print_response);

    // Lectura fuera de rango
    MessageP bad_read = std::make_shared<Message>(MessageType::READ_MEM, -1, 1);
    bad_read->payload.read_mem.address = 4090;
    bad_read->payload.read_mem.size = 32;
    mem.handle_message(bad_read, print_response);

    mem.dump_stats();
    return 0;
}
