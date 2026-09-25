//
//  MemoryBackup.cpp
//
//  Created by MJ (Ruit) on 4/19/20.
//

#include <Includes/Logger.h>
#include <Includes/obfuscate.h>
#include "MemoryBackup.h"


MemoryBackup::MemoryBackup() {
  _address = 0;
  _size    = 0;
  _orig_code.clear();
}

MemoryBackup::MemoryBackup(const char *libraryName, uintptr_t address, size_t backup_size, bool useMapCache) {
  MemoryBackup();

  if (libraryName == NULL || address == 0 || backup_size < 1)
    return;

  _address = KittyMemory::getAbsoluteAddress(libraryName, address, useMapCache);
  if(_address == 0) return;
  
  _size = backup_size;

  _orig_code.resize(backup_size);

  // backup current content
  //
  // 必须检查返回值。旧代码无视结果：读失败时 _orig_code 保持全 0，
  // 而 isValid() 只看 _address/_size/长度，于是一个「空备份」看起来
  // 完好。等 Restore() 把它写回去，目标区域的原字节被清零 ——
  // 游戏当场崩，且崩溃点与原因相距很远。
  if (KittyMemory::memRead(&_orig_code[0], reinterpret_cast<const void *>(_address), backup_size) !=
      Memory_Status::SUCCESS) {
    LOGE("MemoryBackup: 备份失败（地址 %p 可能不可读）", reinterpret_cast<void *>(_address));
    _address = 0;
    _size = 0;
    _orig_code.clear();
  }
}


MemoryBackup::MemoryBackup(uintptr_t absolute_address, size_t backup_size) {
  MemoryBackup();

  if (absolute_address == 0 || backup_size < 1)
    return;

  _address = absolute_address;

  _size = backup_size;

  _orig_code.resize(backup_size);

  // backup current content（同上：读取失败即作废）
  if (KittyMemory::memRead(&_orig_code[0], reinterpret_cast<const void *>(_address), backup_size) !=
      Memory_Status::SUCCESS) {
    LOGE("MemoryBackup: 备份失败（地址 %p 可能不可读）", reinterpret_cast<void *>(_address));
    _address = 0;
    _size = 0;
    _orig_code.clear();
  }
}

   MemoryBackup::~MemoryBackup() {
     // clean up
     _orig_code.clear();
   }


  bool MemoryBackup::isValid() const {
    return (_address != 0 && _size > 0
            && _orig_code.size() == _size);
  }

  size_t MemoryBackup::get_BackupSize() const{
    return _size;
  }

  uintptr_t MemoryBackup::get_TargetAddress() const{
    return _address;
  }

  bool MemoryBackup::Restore() {
    if (!isValid()) return false;
    if (KittyMemory::memWrite(reinterpret_cast<void *>(_address), &_orig_code[0], _size) !=
        Memory_Status::SUCCESS) {
      return false;
    }
    // 备份的通常是代码，写完必须刷指令缓存，否则 CPU 可能仍执行旧字节
    __builtin___clear_cache(reinterpret_cast<char *>(_address),
                            reinterpret_cast<char *>(_address + _size));
    return true;
  }

  std::string MemoryBackup::get_CurrBytes() {
    if (!isValid()) 
      _hexString = std::string(OBFUSCATE("0xInvalid"));
      else 
      _hexString = KittyMemory::read2HexStr(reinterpret_cast<const void *>(_address), _size);

    return _hexString;
  }
