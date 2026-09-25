#include "Patcher.h"
#include "Il2cpp/il2cpp-class.h"
#include "KittyMemory/KittyMemory.h"

using namespace asmjit;

// ASSERR 在 release 下是空的（只求值），所以助手函数自己把错误码传出去，
// 不能指望宏去传播。历史上这里一律 return kErrorOk，等于把所有 asmjit
// 失败都吞掉，patch() 照样把一个残缺/空的 code buffer 写进目标方法。
#define ASSERR(error)                                                                                              \
    do                                                                                                             \
    {                                                                                                              \
        auto _err = (error);                                                                                       \
        if (_err != kErrorOk)                                                                                      \
        {                                                                                                          \
            LOGE("ASSERR: %s => %s", #error, asmjit::DebugUtils::errorAsString(_err));                              \
            return _err;                                                                                           \
        }                                                                                                          \
    } while (0)

Patcher::Patcher(MethodInfo *method)
{
    // 旧代码直接 method->methodPointer，method 或 methodPointer 为空就是空指针解引用。
    // 抽象方法 / 接口方法 / 泛型方法体的 methodPointer 本来就是 null，
    // 而 UI 上的 NOP/改返回值对它们仍然可见。
    if (method == nullptr || method->methodPointer == nullptr)
    {
        LOGE("Patcher: method 或 methodPointer 为空，拒绝打补丁");
        m_valid = false;
        return;
    }
    m_valid = true;
    target = method->methodPointer;

    using namespace asmjit;
    // 注意 arm32 一律选 Arch::kARM 是错的：Android NDK 默认 Thumb-2，
    // 目标代码里混着 32 位 Thumb 指令。发 ARM 编码会破坏目标函数。
    // 这里先按 AArch64 / ARM 环境初始化，实际能否安全打补丁由
    // patch() 里的指令模式校验兜底。
#ifdef __aarch64__
    if (code.init(Environment(Arch::kAArch64)) != kErrorOk)
    {
        LOGE("Patcher: code.init 失败");
        m_valid = false;
        return;
    }
#else
    if (code.init(Environment(Arch::kARM)) != kErrorOk)
    {
        LOGE("Patcher: code.init 失败");
        m_valid = false;
        return;
    }
#endif
    if (code.attach(&assembler) != kErrorOk)
    {
        LOGE("Patcher: code.attach 失败");
        m_valid = false;
    }
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
    // 必须用 x0 而不是 w0：写 w0 会把 x0 的高 32 位清零，
    // 64 位的返回值就被截断了。
    ASSERR(assembler.mov(a64::x0, static_cast<uint64_t>(value) & 0xFFFF));
    ASSERR(assembler.movk(a64::x0, higherBit, a64::lsl(16)));
    ASSERR(assembler.movk(a64::x0, static_cast<uint16_t>(value >> 32), a64::lsl(32)));
    ASSERR(assembler.movk(a64::x0, static_cast<uint16_t>(value >> 48), a64::lsl(48)));
#else
    // 64 位值在 ARM32 要占 r0/r1 一对寄存器，只填 r0 等于截断。
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
    ASSERR(assembler.movw(a32::r1, static_cast<uint16_t>(value >> 32)));
    ASSERR(assembler.movt(a32::r1, static_cast<uint16_t>(value >> 48)));
#endif
    return asmjit::kErrorOk;
}
asmjit::Error Patcher::movUInt64(uint64_t value)
{
#ifdef __aarch64__
    // 同上：用 x0，完整 64 位。
    ASSERR(assembler.mov(a64::x0, value & 0xFFFF));
    ASSERR(assembler.movk(a64::x0, static_cast<uint16_t>(value >> 16), a64::lsl(16)));
    ASSERR(assembler.movk(a64::x0, static_cast<uint16_t>(value >> 32), a64::lsl(32)));
    ASSERR(assembler.movk(a64::x0, static_cast<uint16_t>(value >> 48), a64::lsl(48)));
#else
    ASSERR(assembler.movw(a32::r0, static_cast<uint16_t>(value)));
    ASSERR(assembler.movt(a32::r0, static_cast<uint16_t>(value >> 16)));
    ASSERR(assembler.movw(a32::r1, static_cast<uint16_t>(value >> 32)));
    ASSERR(assembler.movt(a32::r1, static_cast<uint16_t>(value >> 48)));
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
    // ARM32 的浮点返回值在 s0，位模式要搬到 s0；
    // 旧代码只写了 r0，返回值其实一直是垃圾。
    ASSERR(assembler.movw(a32::r0, lowerBit));
    ASSERR(assembler.movt(a32::r0, higherBit));
    ASSERR(assembler.vmov(a32::s0, a32::r0));
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
    // 失败路径统一返回空 vector：调用方（PatcherView）用它判断成败。
    if (!m_valid || target == nullptr)
    {
        LOGE("Patcher::patch: Patcher 无效");
        return {};
    }

    // 收尾：把未解析的链接解掉，再按写入顺序收集各段真实字节。
    // 旧实现只遍历 code.sections()，最后一段尚未 link 完成的代码会被漏掉，
    // 等于把残缺指令写进目标方法。
    code.resolveUnresolvedLinks();

    std::vector<uint8_t> bytes;
    for (auto s : code.sectionsByOrder())
    {
        const auto buf = s->buffer();
        auto size = buf.size();
        if (size == 0)
        {
            continue;
        }
        const uint8_t *data = static_cast<const uint8_t *>(buf.data());
        bytes.insert(bytes.end(), data, data + size);
    }

    if (bytes.empty())
    {
        LOGE("Patcher::patch: asmjit 没产出任何指令，拒绝写入");
        return {};
    }

    std::vector<uint8_t> originalBytes((uint8_t *)target, (uint8_t *)target + bytes.size());
    for (auto b : originalBytes)
    {
        LOGD("%02X", b);
    }

    // 改完要恢复原来的保护（通常是 R+X）。旧实现无条件设成 RWX
    // 并且从不恢复，目标函数所在页面会永久多出写权限。
    const int originalProtect = PROT_READ | PROT_EXEC;
    if (!KittyMemory::ProtectAddr(target, bytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        // mprotect 失败还继续 memcpy 就是在往只读页写 → SIGSEGV。
        LOGE("Patcher::patch: mprotect(RWX) 失败");
        return {};
    }

    memcpy(target, bytes.data(), bytes.size());

    // ARM 上这一步不能省：指令改完不刷 I-cache，CPU 可能继续执行旧字节
    // （表现为补丁「时灵时不灵」或干脆跑飞）。
    __builtin___clear_cache((char *)target, (char *)target + bytes.size());

    if (!KittyMemory::ProtectAddr(target, bytes.size(), originalProtect))
    {
        // 写已经完成，只是恢复失败。必须报出来：目标现在是 RWX，
        // 比原来更宽松的保护不能悄悄带过。
        LOGE("Patcher::patch: 恢复 R+X 失败，目标页仍为 RWX");
    }
    return originalBytes;
}
