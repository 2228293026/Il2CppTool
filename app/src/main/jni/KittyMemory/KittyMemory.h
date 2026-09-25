//
//  KittyMemory.hpp
//
//  Created by MJ (Ruit) on 1/1/19.
//

#pragma once

#include <stdio.h>
#include <string>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>


#define _SYS_PAGE_SIZE_ (sysconf(_SC_PAGE_SIZE))

#define _PAGE_START_OF_(x)    ((uintptr_t)x & ~(uintptr_t)(_SYS_PAGE_SIZE_ - 1))
#define _PAGE_END_OF_(x, len) (_PAGE_START_OF_((uintptr_t)x + len - 1))
#define _PAGE_LEN_OF_(x, len) (_PAGE_END_OF_(x, len) - _PAGE_START_OF_(x) + _SYS_PAGE_SIZE_)
#define _PAGE_OFFSET_OF_(x)   ((uintptr_t)x - _PAGE_START_OF_(x))

#define _PROT_RWX_ (PROT_READ | PROT_WRITE | PROT_EXEC)
#define _PROT_RX_  (PROT_READ | PROT_EXEC)


#define EMPTY_VEC_OFFSET std::vector<int>()

namespace KittyMemory {

    typedef enum {
        FAILED = 0,
        SUCCESS = 1,
        INV_ADDR = 2,
        INV_LEN = 3,
        INV_BUF = 4,
        INV_PROT = 5
    } Memory_Status;


    struct ProcMap {
        // 必须有初值。旧代码这里是裸声明，KittyMemory.cpp 里 `ProcMap ret;`
        // 之后只做条件赋值 —— 库没找到 / sscanf 没匹配上时，startAddr/endAddr
        // 里是栈上的垃圾值，isValid() 判不出来（垃圾 != NULL），于是
        // getAbsoluteAddress 返回「垃圾 + 相对地址」，拿它当函数指针/补丁
        // 目标就是静默改写任意内存。
        void *startAddr = nullptr;
        void *endAddr = nullptr;
        size_t length = 0;
        std::string perms;
        long offset = 0;
        std::string dev;
        int inode = 0;
        std::string pathname;

        bool isValid() const { return (startAddr != NULL && endAddr != NULL && startAddr < endAddr && !pathname.empty()); }

        // relativeAddr 是否落在这个映射内。
        // getAbsoluteAddress 必须过这一关：越界的相对地址算出来的绝对地址
        // 落在别的库里或干脆是野的，调用方拿它去读写就是内存破坏。
        bool contains(uintptr_t relativeAddr) const
        {
            if (!isValid())
                return false;
            uintptr_t start = reinterpret_cast<uintptr_t>(startAddr);
            return relativeAddr < length;
        }
    };

    /*
   * Changes protection of an address with given length
   */
    bool ProtectAddr(void *addr, size_t length, int protection);

    /*
    * Writes buffer content to an address
   */
    Memory_Status memWrite(void *addr, const void *buffer, size_t len);

    /*
   * Reads an address content into a buffer
   */
    Memory_Status memRead(void *buffer, const void *addr, size_t len);

    /*
     * Reads an address content and returns hex string
     */
    std::string read2HexStr(const void *addr, size_t len);


    /*
     * Wrapper to dereference & get value of a multi level pointer
     * Make sure to use the correct data type!
     */
    template<typename Type>
    Type readMultiPtr(void *ptr, std::vector<int> offsets) {
        Type defaultVal = {};
        if (ptr == NULL)
            return defaultVal;

        uintptr_t finalPtr = reinterpret_cast<uintptr_t>(ptr);
        int offsetsSize = offsets.size();
        if (offsetsSize > 0) {
            for (int i = 0; finalPtr != 0 && i < offsetsSize; i++) {
                if (i == (offsetsSize - 1))
                    return *reinterpret_cast<Type *>(finalPtr + offsets[i]);

                finalPtr = *reinterpret_cast<uintptr_t *>(finalPtr + offsets[i]);
            }
        }

        if (finalPtr == 0)
            return defaultVal;

        return *reinterpret_cast<Type *>(finalPtr);
    }


    /*
     * Wrapper to dereference & set value of a multi level pointer
     * Make sure to use the correct data type!, const objects won't work
     */
    template<typename Type>
    bool writeMultiPtr(void *ptr, std::vector<int> offsets, Type val) {
        if (ptr == NULL)
            return false;

        uintptr_t finalPtr = reinterpret_cast<uintptr_t>(ptr);
        int offsetsSize = offsets.size();
        if (offsetsSize > 0) {
            for (int i = 0; finalPtr != 0 && i < offsetsSize; i++) {
                if (i == (offsetsSize - 1)) {
                    *reinterpret_cast<Type *>(finalPtr + offsets[i]) = val;
                    return true;
                }

                finalPtr = *reinterpret_cast<uintptr_t *>(finalPtr + offsets[i]);
            }
        }

        if (finalPtr == 0)
            return false;

        *reinterpret_cast<Type *>(finalPtr) = val;
        return true;
    }

    /*
     * Wrapper to dereference & get value of a pointer
     * Make sure to use the correct data type!
     */
    template<typename Type>
    Type readPtr(void *ptr) {
        Type defaultVal = {};
        if (ptr == NULL)
            return defaultVal;

        return *reinterpret_cast<Type *>(ptr);
    }

    /*
     * Wrapper to dereference & set value of a pointer
     * Make sure to use the correct data type!, const objects won't work
     */
    template<typename Type>
    bool writePtr(void *ptr, Type val) {
        if (ptr == NULL)
            return false;

        *reinterpret_cast<Type *>(ptr) = val;
        return true;
    }

    /*
     * Gets info of a mapped library in self process
     */
    ProcMap getLibraryMap(const char *libraryName);

    /*
     * 找到包含 [addr, addr+len) 的那个映射。
     * 用途：在拿一个算出来的/缓存的地址去读写之前，确认它真的落在
     * 某个可读/可写映射里。野指针直接 memcpy 会 SIGSEGV，
     * 而在 Android 上查 /proc/self/maps 是唯一稳妥的探测手段。
     * 找不到返回无效的 ProcMap（isValid() 为 false）。
     */
    ProcMap getContainingMap(const void *addr, size_t len);

    /*
    * Expects a relative address in a library
    * Returns final absolute address
    */
    uintptr_t
    getAbsoluteAddress(const char *libraryName, uintptr_t relativeAddr, bool useCache = false);
};
