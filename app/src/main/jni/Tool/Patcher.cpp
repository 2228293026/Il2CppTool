#include "Patcher.h"
#include "Il2cpp/il2cpp-class.h"
#include "KittyMemory/KittyMemory.h"

using namespace asmjit;

#ifdef __DEBUG__
    #define ASSERR(error)                                                                                              \
        do                                                                                                             \
        {                                                                                                              \
            if (error != kErrorOk)                                                                                     \
                LOGE("ASSERR: %s => %s", #error, asmjit::DebugUtils::errorAsString(error));                            \
        } while (0)
#else
    #define ASSERR(error) (error)
#endif

Patcher::Patcher(MethodInfo *method) : target{method->methodPointer}
{
    using namespace asmjit;
#ifdef __aarch64__
    code.init(Environment(Arch::kAArch64));
#else
    code.init(Environment(Arch::kARM));
#endif
    code.attach(&assembler);
}
Error Patcher::ret()
{
#ifdef __aarch64__
    ASSERR(assembler.ret(asmjit::a64::x30));
#else
    ASSERR(assembler.bx(asmjit::a32::lr));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movInt16(int16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

Error Patcher::movUInt16(uint16_t value)
{
#ifdef __aarch64__
    ASSERR(assembler.movz(a64::x0, value));
#else
    ASSERR(assembler.movw(a32::r0, value));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt32(int32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt32(uint32_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movInt64(int64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt64(uint64_t value)
{
    uint16_t lowerBit = static_cast<uint16_t>(value);
    uint16_t higherBit = static_cast<uint16_t>(value >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movFloat(float value)
{
    union FloatBits
    {
        float f;
        uint32_t i;
    };
    FloatBits fb{value};

    uint16_t lowerBit = static_cast<uint16_t>(fb.i);
    uint16_t higherBit = static_cast<uint16_t>(fb.i >> 16);
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::w0, lowerBit));
    ASSERR(assembler.movk(a64::w0, higherBit, a64::lsl(16)));
    ASSERR(assembler.fmov(a64::s0, a64::w0));
#else
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movBool(bool value)
{
    int b = value ? 1 : 0;
#ifdef __aarch64__
    ASSERR(assembler.mov(a64::x0, b));
#else
    ASSERR(assembler.mov(a32::r0, b));
#endif
    return asmjit::kErrorOk;
}

asmjit::Error Patcher::movPtr(void *value)
{
#ifdef __aarch64__
    auto val = imm(value);
    ASSERR(assembler.mov(a64::x0, val));
#else
    uint16_t lowerBit = static_cast<uint16_t>((uintptr_t)value);
    uint16_t higherBit = static_cast<uint16_t>((uintptr_t)value >> 16);
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
#endif
    return asmjit::kErrorOk;
}

std::vector<uint8_t> Patcher::patch()
{
    std::vector<char> bytes;
    for (auto s : code.sections())
    {
        for (auto c : s->buffer())
        {
            LOGD("%02X", c);
            bytes.push_back(c);
        }
    }

    auto rawBytes = bytes.data();
    auto protect = KittyMemory::ProtectAddr(target, bytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC);
    LOGINT(protect);

    std::vector<uint8_t> originalBytes((uint8_t *)target, (uint8_t *)target + bytes.size());
    for (auto b : originalBytes)
    {
        LOGD("%02X", b);
    }

    memcpy(target, (void *)rawBytes, bytes.size());
    return originalBytes;
}
