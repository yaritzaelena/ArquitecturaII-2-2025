#include "pe.hpp"
#include <thread>
#include <chrono>
#include <iostream>



PE::PE(int id, IMemoryPort* mem) : id_(id), mem_(mem) {
    R_.fill(0);
}

void PE::load_program(const Program& p) {
    prog_ = p; pc_ = 0;
}

void PE::set_segment(uint64_t baseA, uint64_t baseB, uint64_t partial_out, uint64_t len_quarter) {
    R_[0] = 0;            // i
    R_[1] = baseA;        // base A
    R_[2] = baseB;        // base B
    R_[3] = double_as_u64(0.0); // acc
    R_[7] = len_quarter;  // limit
    R_[5] = partial_out;  // reuse para store final
}

void PE::run(uint64_t max_steps) {
    uint64_t steps = 0;
    bool halted = false;
    while (!halted) {
        if (mem_) mem_->service(); 
        step(halted);
        if (max_steps && ++steps >= max_steps) break;
    }
}

void PE::step(bool& halted) {
    if (pc_ >= prog_.size()) { halted = true; return; }
    const Instr& I = prog_[pc_];

    switch (I.op) {
        case Op::HALT: halted = true; break;

        case Op::LOAD: {
            uint64_t addr = R_[I.a];
            uint64_t val  = mem_->load64(addr);
            R_[I.d] = val;
            pc_++;
        } break;

        case Op::STORE: {
            uint64_t addr = R_[I.a];
            mem_->store64(addr, R_[I.d]);
            pc_++;
        } break;

        case Op::FMUL: {
            double a = u64_as_double(R_[I.a]);
            double b = u64_as_double(R_[I.b]);
            R_[I.d] = double_as_u64(a*b);
            pc_++;
        } break;

        case Op::FADD: {
            double a = u64_as_double(R_[I.a]);
            double b = u64_as_double(R_[I.b]);
            R_[I.d] = double_as_u64(a+b);
            pc_++;
        } break;

        case Op::INC:  R_[I.d]++; pc_++; break;
        case Op::DEC:  R_[I.d]--; pc_++; break;

        case Op::JNZ: {
            if (R_[I.d] != 0) pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + I.imm);
            else pc_++;
        } break;

        case Op::LEA: {
            R_[I.d] = R_[I.a] + (R_[I.b] << I.imm);
            pc_++;
        } break;
    }
}




