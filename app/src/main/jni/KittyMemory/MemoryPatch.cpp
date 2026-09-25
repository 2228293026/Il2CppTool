//
//  MemoryPatch.cpp
//
//  Created by MJ (Ruit) on 1/1/19.
//

#include <Includes/obfuscate.h>
#include "MemoryPatch.h"
#include "Includes/Logger.h"

MemoryPatch::MemoryPatch() {
    _address = 0;
    _size = 0;
    _orig_code.clear();
    _patch_code.clear();
}

MemoryPatch::MemoryPatch(const char *libraryName, uintptr_t address,
                         const void *patch_code, size_t patch_size, bool useMapCache) {
    MemoryPatch();

    if (libraryName == NULL || address == 0 || patch_code == NULL || patch_size < 1)
        return;

    _address = KittyMemory::getAbsoluteAddress(libraryName, address, useMapCache);
    if (_address == 0) return;

    _size = patch_size;

    _orig_code.resize(patch_size);
    _patch_code.resize(patch_size);

    // initialize patch & backup current content
    //
    // 必须检查返回值。旧代码两个 memRead 都不看结果：读失败时
    // _orig_code 保持全 0，而 isValid() 只看 _size/地址，于是这个
    // 「备份」看起来完全正常。等到 restore() 把这堆 0 写回去，
    // 目标函数的机器码被清零 —— 游戏当场崩，而且崩溃点离真正的原因
    // 很远，极难排查。
    if (KittyMemory::memRead(&_patch_code[0], patch_code, patch_size) != Memory_Status::SUCCESS) {
        LOGE("MemoryPatch: 读取补丁代码失败");
        _address = 0;
        _size = 0;
        _orig_code.clear();
        _patch_code.clear();
        return;
    }
    if (KittyMemory::memRead(&_orig_code[0], reinterpret_cast<const void *>(_address), patch_size) !=
        Memory_Status::SUCCESS) {
        LOGE("MemoryPatch: 备份原代码失败（地址 %p 可能不可读）", reinterpret_cast<void *>(_address));
        _address = 0;
        _size = 0;
        _orig_code.clear();
        _patch_code.clear();
    }
}

MemoryPatch::MemoryPatch(uintptr_t absolute_address,
                         const void *patch_code, size_t patch_size) {
    MemoryPatch();

    if (absolute_address == 0 || patch_code == NULL || patch_size < 1)
        return;

    _address = absolute_address;
    _size = patch_size;

    _orig_code.resize(patch_size);
    _patch_code.resize(patch_size);

    // initialize patch & backup current content（同上：必须检查返回值）
    if (KittyMemory::memRead(&_patch_code[0], patch_code, patch_size) != Memory_Status::SUCCESS) {
        LOGE("MemoryPatch: 读取补丁代码失败");
        _address = 0;
        _size = 0;
        _orig_code.clear();
        _patch_code.clear();
        return;
    }
    if (KittyMemory::memRead(&_orig_code[0], reinterpret_cast<const void *>(_address), patch_size) !=
        Memory_Status::SUCCESS) {
        LOGE("MemoryPatch: 备份原代码失败（地址 %p 可能不可读）", reinterpret_cast<void *>(_address));
        _address = 0;
        _size = 0;
        _orig_code.clear();
        _patch_code.clear();
    }
}

MemoryPatch::~MemoryPatch() {
    // clean up
    _orig_code.clear();
    _patch_code.clear();
}

MemoryPatch MemoryPatch::createWithHex(const char *libraryName, uintptr_t address,
                                       std::string hex, bool useMapCache) {
    MemoryPatch patch;

    if (libraryName == NULL || address == 0 || !KittyUtils::validateHexString(hex))
        return patch;

    patch._address = KittyMemory::getAbsoluteAddress(libraryName, address, useMapCache);
    if (patch._address == 0) return patch;

    patch._size = hex.length() / 2;

    patch._orig_code.resize(patch._size);
    patch._patch_code.resize(patch._size);

    // initialize patch
    KittyUtils::fromHex(hex, &patch._patch_code[0]);

    // backup current content
    // 读取失败必须让这个 patch 失效：备份不全的 patch 一旦 restore，
    // 目标位置会被写进「一半原字节一半 0」，结果是随机崩溃，
    // 而且崩溃点离真正的原因非常远。
    if (KittyMemory::memRead(&patch._orig_code[0], reinterpret_cast<const void *>(patch._address),
                             patch._size) != Memory_Status::SUCCESS) {
        LOGE("createWithHex: 备份原代码失败（地址 %p 可能不可读）", reinterpret_cast<void *>(patch._address));
        patch._address = 0;
        patch._size = 0;
        patch._orig_code.clear();
        patch._patch_code.clear();
    }
    return patch;
}

MemoryPatch MemoryPatch::createWithHex(uintptr_t absolute_address, std::string hex) {
    MemoryPatch patch;

    if (absolute_address == 0 || !KittyUtils::validateHexString(hex))
        return patch;

    patch._address = absolute_address;
    patch._size = hex.length() / 2;

    patch._orig_code.resize(patch._size);
    patch._patch_code.resize(patch._size);

    // initialize patch
    KittyUtils::fromHex(hex, &patch._patch_code[0]);

    // backup current content（同上：读取失败即作废）
    if (KittyMemory::memRead(&patch._orig_code[0], reinterpret_cast<const void *>(patch._address),
                             patch._size) != Memory_Status::SUCCESS) {
        LOGE("createWithHex: 备份原代码失败（地址 %p 可能不可读）", reinterpret_cast<void *>(patch._address));
        patch._address = 0;
        patch._size = 0;
        patch._orig_code.clear();
        patch._patch_code.clear();
    }
    return patch;
}

bool MemoryPatch::isValid() const {
    return (_address != 0 && _size > 0
            && _orig_code.size() == _size && _patch_code.size() == _size);
}

size_t MemoryPatch::get_PatchSize() const {
    return _size;
}

uintptr_t MemoryPatch::get_TargetAddress() const {
    return _address;
}

bool MemoryPatch::Restore() {
    if (!isValid()) return false;
    //LOGI("Restore %i", isLeeched);
    if (KittyMemory::memWrite(reinterpret_cast<void *>(_address), &_orig_code[0], _size) !=
        Memory_Status::SUCCESS) {
        return false;
    }
    // 刷指令缓存：memWrite 只做了 mprotect + memcpy，没有这一步。
    // ARM 上 CPU 可能继续执行旧字节，表现为「恢复了但行为没变」，
    // 或者更糟 —— 半新半旧的指令流直接跑飞。
    __builtin___clear_cache(reinterpret_cast<char *>(_address),
                            reinterpret_cast<char *>(_address + _size));
    return true;
}

bool MemoryPatch::Modify() {
    if (!isValid()) return false;
    //LOGI("Modify");
    if (KittyMemory::memWrite(reinterpret_cast<void *>(_address), &_patch_code[0], _size) !=
        Memory_Status::SUCCESS) {
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char *>(_address),
                            reinterpret_cast<char *>(_address + _size));
    return true;
}

std::string MemoryPatch::get_CurrBytes() {
    if (!isValid())
        _hexString = std::string(OBFUSCATE("0xInvalid"));
    else
        _hexString = KittyMemory::read2HexStr(reinterpret_cast<const void *>(_address), _size);

    return _hexString;
}
