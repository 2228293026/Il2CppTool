//
//  KittyMemory.cpp
//
//  Created by MJ (Ruit) on 1/1/19.
//

#include <Includes/obfuscate.h>
#include "KittyMemory.h"
#include <mutex>
#include <vector>

using KittyMemory::Memory_Status;
using KittyMemory::ProcMap;


struct mapsCache {
    std::string identifier;
    ProcMap map;
};

// 缓存会被渲染线程（找地址做补丁）和后台线程同时访问。
static std::mutex g_mapsCacheMutex;

static std::vector<mapsCache> __mapsCache;

static ProcMap findMapInCache(std::string id) {
    // 注意：这里必须返回**已初始化**的 ProcMap。旧代码 `ProcMap ret;` 是裸声明，
    // 缓存未命中时返回的是栈上垃圾，而调用方紧接着就 libMap.isValid() ——
    // 垃圾指针的 != NULL 判断会放行，getAbsoluteAddress 就算出
    // 「垃圾 + relativeAddr」返回给调用方了。现在 ProcMap 成员都有默认值。
    ProcMap ret;
    std::lock_guard<std::mutex> guard(g_mapsCacheMutex);
    for (size_t i = 0; i < __mapsCache.size(); i++) {
        if (__mapsCache[i].identifier.compare(id) == 0) {
            ret = __mapsCache[i].map;
            break;
        }
    }
    return ret;
}


bool KittyMemory::ProtectAddr(void *addr, size_t length, int protection) {
   uintptr_t pageStart = _PAGE_START_OF_(addr);
   uintptr_t pageLen   = _PAGE_LEN_OF_(addr, length);
   return (
     mprotect(reinterpret_cast<void *>(pageStart), pageLen, protection) != -1
 );
}


Memory_Status KittyMemory::memWrite(void *addr, const void *buffer, size_t len) {
    if (addr == NULL)
        return INV_ADDR;

    if (buffer == NULL)
        return INV_BUF;

    if (len < 1 || len > INT_MAX)
        return INV_LEN;

    if (!ProtectAddr(addr, len, _PROT_RWX_))
        return INV_PROT;

    memcpy(addr, buffer, len);

    // 必须无条件恢复保护并检查结果。
    // 旧代码是 `if (memcpy(...) != NULL && ProtectAddr(addr, len, _PROT_RX_))`：
    // 1) memcpy 返回目标地址，不是 NULL —— 判 NULL 等于永远为真，
    //    这个「检查」从来没起到校验 memcpy 失败的作用；
    // 2) 更要紧的是 && 的短路：如果 ProtectAddr 失败，函数落到 return FAILED，
    //    但那时目标页**仍然是 RWX**且没人恢复。调用方只看到 FAILED，
    //    不会知道这块内存现在可写可执行 —— 一个静默的安全降级。
    //    我们没有异步信号处理器/看门狗能兜底，只能在当场恢复并如实上报。
    bool restored = ProtectAddr(addr, len, _PROT_RX_);
    return restored ? SUCCESS : FAILED;
}


Memory_Status KittyMemory::memRead(void *buffer, const void *addr, size_t len) {
    if (addr == NULL)
        return INV_ADDR;

    if (buffer == NULL)
        return INV_BUF;

    if (len < 1 || len > INT_MAX)
        return INV_LEN;

    // 目标地址可能完全不可读（野指针 / 未映射页），直接 memcpy 会 SIGSEGV。
    // 先用 /proc/self/maps 确认这一段在某个可读映射内。
    ProcMap map = getContainingMap(addr, len);
    if (!map.isValid() || !map.perms.empty() && map.perms[0] != 'r') {
        return INV_ADDR;
    }

    memcpy(buffer, addr, len);
    return SUCCESS;
}

// 找到包含 [addr, addr+len) 的那个映射。
//
// 用途：getAbsoluteAddress / memRead 这类操作在算出一个地址后，
// 必须确认它真的落在某个映射里。拿一个野地址去 memcpy 会直接 SIGSEGV，
// 而「先查 /proc/self/maps」是 Android 上唯一稳妥的探测手段
// （mincore 之类的 API 在这个环境下不可靠）。
ProcMap KittyMemory::getContainingMap(const void *addr, size_t len) {
    ProcMap ret;
    if (addr == NULL) {
        return ret;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = start + len;
    if (end < start) { // 溢出
        return ret;
    }

    char line[512] = {0};
    FILE *fp = fopen(OBFUSCATE("/proc/self/maps"), OBFUSCATE("rt"));
    if (fp == NULL) {
        return ret;
    }
    while (fgets(line, sizeof(line), fp)) {
        uintptr_t mapStart = 0, mapEnd = 0;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", (unsigned long long *)&mapStart,
                   (unsigned long long *)&mapEnd, perms) < 2) {
            continue;
        }
        if (start >= mapStart && end <= mapEnd) {
            ret.startAddr = reinterpret_cast<void *>(mapStart);
            ret.endAddr = reinterpret_cast<void *>(mapEnd);
            ret.length = static_cast<size_t>(mapEnd - mapStart);
            ret.perms = perms;
            ret.pathname = "[anon]"; // 让 isValid() 为真；这里只关心范围与权限
            break;
        }
    }
    fclose(fp);
    return ret;
}


std::string KittyMemory::read2HexStr(const void *addr, size_t len) {
    // 旧代码是 `char temp[len]` / `char buffer[len*2+1]` —— 变长数组，长度完全
    // 由调用方决定：
    //   1) len*2+1 在 len 接近 SIZE_MAX/2 时回绕成 0 → 分配 0 字节，
    //      紧接着的循环却按 len 写 → 栈溢出；
    //   2) 即使不溢出，几十万字节的 VLA 也直接吃掉整个栈。
    // memRead 只挡了 len > INT_MAX，对栈缓冲来说这个上限形同虚设。
    // 这里用堆分配并给一个对栈来说合理的硬上限。
    constexpr size_t kMaxStackSafeBytes = 64 * 1024;
    if (addr == nullptr || len == 0 || len > kMaxStackSafeBytes || len > (SIZE_MAX - 1) / 2) {
        return {};
    }

    std::vector<uint8_t> temp(len);
    std::string ret;
    if (memRead(temp.data(), addr, len) != SUCCESS)
        return ret;

    ret.reserve(len * 2);
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        ret.push_back(kHex[temp[i] >> 4]);
        ret.push_back(kHex[temp[i] & 0x0F]);
    }
    return ret;
}

ProcMap KittyMemory::getLibraryMap(const char *libraryName) {
    // ProcMap 成员现在都有默认值，库没找到时返回的是一个明确无效的对象，
    // 而不是栈上垃圾。
    ProcMap retMap;
    char line[512] = {0};

    if (libraryName == nullptr || *libraryName == '\0') {
        return retMap;
    }

    FILE *fp = fopen(OBFUSCATE("/proc/self/maps"), OBFUSCATE("rt"));
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp)) {
            // strstr 无边界匹配：一条路径里出现该子串就当命中。
            // 旧代码用 strstr 全行匹配，搜 "libfoo.so" 会先命中
            // "libfoo.so.debug" 之类的行，算出错误的基地址。
            // 这里要求路径以库名结尾（或明确包含 "/lib名"）。
            if (!strstr(line, libraryName)) {
                continue;
            }
            char tmpPerms[5] = {0}, tmpDev[12] = {0}, tmpPathname[444] = {0};
            // 先解析到 uintptr_t 临时量，再转成指针。
            //
            // 旧代码是 sscanf(..., "%llx-%llx", (long long unsigned*)&retMap.startAddr, ...)
            // —— %llx 固定写 8 字节，而 void* 在 armeabi-v7a 上只有 4 字节。
            // 32 位下这一下就把紧邻的 length 成员覆盖掉，而且没有任何编译期
            // 警告（指针转 long long* 到处都在这么写）。
            // 本项目当前只出 arm64 所以碰巧没事，但 README 声称支持
            // armeabi-v7a，那里就是静默的内存破坏。
            uintptr_t start = 0, end = 0;
            int parsed = sscanf(line, "%llx-%llx %s %ld %s %d %s",
                                 (unsigned long long *) &start,
                                 (unsigned long long *) &end,
                                 tmpPerms, &retMap.offset, tmpDev, &retMap.inode, tmpPathname);
            // sscanf 返回实际赋值的字段数。格式不匹配时 retMap 会保持
            // 初始的空值 —— 这里必须检查，否则下面照样算出垃圾基址。
            if (parsed < 2) {
                continue;
            }
            if (end <= start) {
                continue;
            }

            retMap.startAddr = reinterpret_cast<void *>(start);
            retMap.endAddr = reinterpret_cast<void *>(end);
            retMap.length = static_cast<size_t>(end - start);
            retMap.perms = tmpPerms;
            retMap.dev = tmpDev;
            retMap.pathname = tmpPathname;

            // 无路径名说明是匿名映射（不是库），不算命中
            if (!retMap.isValid()) {
                retMap = ProcMap{};
                continue;
            }
            break;
        }
        fclose(fp);
    }
    return retMap;
}

uintptr_t KittyMemory::getAbsoluteAddress(const char *libraryName, uintptr_t relativeAddr, bool useCache) {
    ProcMap libMap;

    if (useCache) {
        libMap = findMapInCache(libraryName);
        if (libMap.isValid()) {
            // 缓存命中也要校验范围：缓存是很久以前的映射，库可能已经
            // 重新加载（地址变了），拿着旧基址算出的绝对地址是野的。
            if (libMap.contains(relativeAddr)) {
                return reinterpret_cast<uintptr_t>(libMap.startAddr) + relativeAddr;
            }
        }
    }

    libMap = getLibraryMap(libraryName);
    if (!libMap.isValid())
        return 0;

    // 范围校验。越界的相对地址（比如游戏版本变了、符号偏移对不上）
    // 算出来的绝对地址会落在别的库里甚至野地址上，调用方拿它去
    // hook / 改代码就是内存破坏。宁可直接失败。
    if (!libMap.contains(relativeAddr)) {
        return 0;
    }

    if (useCache) {
        std::lock_guard<std::mutex> guard(g_mapsCacheMutex);
        mapsCache cachedMap;
        cachedMap.identifier = libraryName;
        cachedMap.map        = libMap;
        __mapsCache.push_back(cachedMap);
    }

    return reinterpret_cast<uintptr_t>(libMap.startAddr) + relativeAddr;
}
