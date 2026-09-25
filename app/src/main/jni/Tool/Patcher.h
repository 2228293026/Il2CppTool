#pragma once
#include "asmjit.h"
#ifdef __aarch64__
    #include "a64.h"
#else
    #include "a32.h"
#endif
#include <vector>
#include <cstdint>

struct MethodInfo;

class Patcher
{
  public:
    Patcher(MethodInfo *method);

    asmjit::Error ret();
    asmjit::Error movInt16(int16_t value);
    asmjit::Error movUInt16(uint16_t value);

    asmjit::Error movInt32(int32_t value);
    asmjit::Error movUInt32(uint32_t value);

    // 64 位：arm64 走 x0 的四个 16 位片段，arm32 走 r0/r1 寄存器对。
    asmjit::Error movInt64(int64_t value);
    asmjit::Error movUInt64(uint64_t value);

    asmjit::Error movFloat(float value);

    asmjit::Error movBool(bool value);

    asmjit::Error movPtr(void *value);

    // 成功返回原字节（用于 restore），失败返回空 vector。
    std::vector<uint8_t> patch();

    // method 或 methodPointer 为空、asmjit 初始化失败时为 false，
    // 此时 patch() 会直接失败而不是往空地址写。
    bool valid() const
    {
        return m_valid;
    }

  private:
    asmjit::CodeHolder code;
#ifdef __aarch64__
    asmjit::a64::Assembler assembler;
#else
    asmjit::a32::Assembler assembler;
#endif
    void *target = nullptr;
    bool m_valid = false;
};
