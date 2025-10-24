
#ifdef PE_STANDALONE
#include <vector>
#include <cstring>
#include <cassert>
#include "pe.hpp"
#include <vector>
#include <cstring>
#include <iostream>
#include <cassert>
#include <cmath>



class TrivialMem : public IMemoryPort {
public:
    explicit TrivialMem(size_t bytes) : mem(bytes, 0) {}

    uint64_t load64(uint64_t addr) override {
        uint64_t u=0; for (int i=0;i<8;i++) u |= (uint64_t)mem[addr+i] << (i*8);
        return u;
    }
    void store64(uint64_t addr, uint64_t val) override {
        for (int i=0;i<8;i++) mem[addr+i] = (uint8_t)((val >> (i*8)) & 0xFF);
    }
    void service() override {} // nada que hacer

    void write_double(size_t addr, double v) {
        uint64_t u; std::memcpy(&u, &v, 8);
        for (int i=0;i<8;i++) mem[addr+i] = (uint8_t)((u >> (i*8)) & 0xFF);
    }
    double read_double(size_t addr) const {
        uint64_t u=0; for (int i=0;i<8;i++) u |= (uint64_t)mem[addr+i] << (i*8);
        double d; std::memcpy(&d, &u, 8); return d;
    }

private:
    std::vector<uint8_t> mem;
};

static Program make_dot_program() {
    Program p;

    // R0 = i, R1 = baseA, R2 = baseB, R3 = acc, R5 = partial_out, R7 = limit
    // Temporales: R4 y R6

    p.push_back({Op::LEA, 4,1,0,3});   // R4 = &A[i] = R1 + (R0<<3)
    p.push_back({Op::LEA, 6,2,0,3});   // R6 = &B[i] = R2 + (R0<<3)
    p.push_back({Op::LOAD,4,4,0,0});   // R4 = A[i]
    p.push_back({Op::LOAD,6,6,0,0});   // R6 = B[i]
    p.push_back({Op::FMUL,4,4,6,0});   // R4 = A[i] * B[i]
    p.push_back({Op::FADD,3,3,4,0});   // acc += R4
    p.push_back({Op::INC, 0,0,0,0});   // i++
    p.push_back({Op::DEC, 7,0,0,0});   // limit--
    p.push_back({Op::JNZ, 7,0,0,-8});  // loop si R7 != 0

    // Guardar el acumulado en la dirección partial_out (R5)
    p.push_back({Op::STORE,3,5,0,0});  // [R5] = acc
    p.push_back({Op::HALT,0,0,0,0});
    return p;
}

int main() {
    const size_t N = 16;
    const size_t baseA = 0;
    const size_t baseB = baseA + N*8;
    const size_t baseP = baseB + N*8;

    TrivialMem mem(4096);
    for (size_t i=0;i<N;i++) {
        mem.write_double(baseA + i*8, double(i+1));          // A = [1..N]
        mem.write_double(baseB + i*8, double((i+1)*0.5));    // B = [0.5,1.0,1.5..]
    }

    PE pe0(0, &mem);
    pe0.load_program(make_dot_program());
    pe0.set_segment(baseA, baseB, baseP, N); // aquí N completo, en real usar N/4
    pe0.run();

    double partial = mem.read_double(baseP);
    double expected = 0.5 * (N*(N+1)*(2*N+1)/6.0);
    std::cout << "Partial (PE0) = " << partial << "\\n";
    std::cout << "Expected      = " << expected << "\\n";
}
#endif
