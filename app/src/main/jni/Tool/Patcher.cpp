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

// 在目标方法的原始字节里找出**第一条** ret 的偏移；找不到返回 -1。
//
// arm64 的 ret 只有一个标准编码：D65F03C0（小端字节序 C0 03 5F D6）。
// 另有一族别名编码（ret xN 等价写法），这里不追求完备 —— 判不出来时
// patch() 会保守地拒绝打补丁，宁可不给这个功能也不要悄悄改坏别的方法。
#ifdef __aarch64__
static long long FindFirstRetArm64(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        if (p[i] == 0xC0 && p[i + 1] == 0x03 && p[i + 2] == 0x5F && p[i + 3] == 0xD6)
        {
            return static_cast<long long>(i);
        }
    }
    return -1;
}
#endif

// 判断我们生成的 stub 是否**完整**。
//
// asmjit 的指令发射可能失败：分配器在内存吃紧的游戏里申请失败就是现实场景。
// 而 CodeHolder 一旦进入错误状态，后续所有 emit 都会失败，于是缓冲区里可能
// 只剩半条指令（比如光一个 movz），长度甚至比一条完整指令还短 —— 这种残缺
// stub 被写进方法开头，方法既没有正确返回值也没有 ret，直接顺着自己的
// 原始指令流跑下去，行为完全不可预测。
//
// 所以必须验证最后一条确实是 ret：一条「改返回值」的 stub 不可能不以 ret 收尾。
static bool StubEndsWithRet(const std::vector<uint8_t> &bytes)
{
#ifdef __aarch64__
    if (bytes.size() < 4)
    {
        return false;
    }
    const size_t last = bytes.size() - 4;
    return bytes[last] == 0xC0 && bytes[last + 1] == 0x03 && bytes[last + 2] == 0x5F &&
           bytes[last + 3] == 0xD6;
#else
    // arm32 未支持打补丁（见构造函数里的说明），保守判否。
    return false;
#endif
}

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

    if (!StubEndsWithRet(bytes))
    {
        // 说明 emit 过程中出过错，缓冲区是残缺的。
        // 写进去 = 方法没有正确的返回值也没有 ret，会顺着自己的原始指令流
        // 一路跑下去。宁可拒绝。
        LOGE("Patcher::patch: 生成的 stub 未以 ret 收尾（%zu 字节），"
             "很可能 asmjit 发射失败，拒绝写入",
             bytes.size());
        return {};
    }

    // ---- 关键安全检查：stub 必须能装进这个方法里 ----
    //
    // il2cpp 把所有方法体**连续排布**在同一个 RX 段里，彼此紧挨着。
    // arm64 上一个 stub 最长 20 字节（movPtr+ret / movInt64+ret），
    // 而 il2cpp 里有大量 4~12 字节的方法体（ret / ldr w0,[x0,#8]; ret 这种）。
    //
    // 旧代码唯一的检查是 bytes.empty()，于是给一个 8 字节的方法打 20 字节的
    // 补丁时，会把**下一个方法的序言**一起覆盖掉。用户当场看着补丁"生效了"，
    // 然后在毫不相干的地方玩着玩着游戏就崩了 —— 崩点离真正的原因十万八千里。
    //
    // 做法是读原始字节，找到第一条 ret：原方法至少到那条 ret 为止。
    // 我们的 stub 必须在那条 ret 结束之前或正好结束，否则就会溢出到邻居。
#ifdef __aarch64__
    {
        constexpr size_t kScanWindow = 64; // 足够短，也不至于扫到无关代码
        // 旧代码写的是 `std::min(kScanWindow, bytes.size())` —— 但 bytes 是
        // **我们生成的 stub**（movPtr+ret 之类，最多 20 字节），不是目标方法。
        // 于是 scanLen 实际等于 stub 长度（约 20），扫不满 64 字节。
        //
        // 后果：序言稍长一点、`ret` 在偏移 24/28 处的常见方法，会被判成
        // 「找不到 ret」而**拒绝打补丁** —— 哪怕补丁明明装得下。
        // 清单 5.2（给较大方法打补丁应生效）会直接失败。
        //
        // 这里就是要读**目标方法**的前若干字节，和 stub 大小无关。
        //
        // 另外要夹到「本页还剩多少字节」：方法体都在同一个 RX 段里，但
        // **最后几个方法**可能紧贴映射末尾。直接读 64 字节会跨进未映射页
        // → SIGSEGV，而 Patcher 是本项目风险最高的子系统，崩溃在这里最难查。
        const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
        const uintptr_t pageEnd = (addr & ~(static_cast<uintptr_t>(4095))) + 4096;
        size_t scanLen = kScanWindow;
        if (addr + scanLen > pageEnd)
        {
            scanLen = static_cast<size_t>(pageEnd - addr);
        }
        if (scanLen < 4)
        {
            LOGE("Patcher::patch: 方法起始处剩余可读字节不足 4，无法确认边界，拒绝写入");
            return {};
        }
        const long long retOffset = FindFirstRetArm64((const uint8_t *)target, scanLen);
        if (retOffset < 0)
        {
            LOGE("Patcher::patch: 在前 %zu 字节里找不到 ret，无法确认方法边界，拒绝写入", scanLen);
            return {};
        }
        const long long methodEnd = retOffset + 4; // ret 自身占 4 字节
        if (static_cast<long long>(bytes.size()) > methodEnd)
        {
            LOGE("Patcher::patch: stub(%zu 字节)装不下这个方法(到 ret 为止仅 %lld 字节)，"
                 "写进去会覆盖下一个方法 —— 拒绝",
                 bytes.size(), methodEnd);
            return {};
        }
    }
#else
    LOGE("Patcher::patch: 非 arm64 平台不做方法边界校验，拒绝写入");
    return {};
#endif

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
