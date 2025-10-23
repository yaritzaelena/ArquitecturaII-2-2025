#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <cstring>


class IMemoryPort {
public:
    virtual ~IMemoryPort() = default;
    virtual uint64_t load64(uint64_t addr) = 0;
    virtual void     store64(uint64_t addr, uint64_t val) = 0;
    virtual void     service() = 0; 
};

enum class Op : uint8_t {
    LOAD, STORE, FMUL, FADD, INC, DEC, JNZ, HALT,
    LEA,           // Rd = Ra + (Rb << imm)  ( dir. efectivas A[i],B[i])
};

struct Instr {
    Op op;
    uint8_t d=0, a=0, b=0;
    int64_t imm=0;         
};

using Program = std::vector<Instr>;


class PE {
public:
    explicit PE(int id, IMemoryPort* mem);

    void load_program(const Program& p);

    void set_segment(uint64_t baseA, uint64_t baseB, uint64_t partial_out, uint64_t len_quarter);

    void run(uint64_t max_steps = 0);

    const std::array<uint64_t,8>& regs() const { return R_; }

private:
    int id_ = 0;
    IMemoryPort* mem_ = nullptr;
    Program prog_;
    uint64_t pc_ = 0;
    std::array<uint64_t,8> R_{}; // 8 x 64-bit


    void step(bool& halted);
    static inline double u64_as_double(uint64_t u) {
        double d; std::memcpy(&d, &u, 8); return d;
    }
    static inline uint64_t double_as_u64(double d) {
        uint64_t u; std::memcpy(&u, &d, 8); return u;
    }
};


