/* --------------------------------- ABOUT -------------------------------------
Original Author: Adam Yaxley
Website: https://github.com/adamyaxley
License: See end of file
Obfuscate
Guaranteed compile-time string literal obfuscation library for C++14
Usage:
Pass string literals into the AY_OBFUSCATE macro to obfuscate them at compile
time. AY_OBFUSCATE returns a reference to an ay::obfuscated_data object with the
following traits:
  - Guaranteed obfuscation of string
  The passed string is encrypted with a simple XOR cipher at compile-time to
  prevent it being viewable in the binary image
  - Global lifetime
  The actual instantiation of the ay::obfuscated_data takes place inside a
  lambda as a function level static
  - Implicitly convertable to a char*
  This means that you can pass it directly into functions that would normally
  take a char* or a const char*
Example:
const char* obfuscated_string = AY_OBFUSCATE("Hello World");
std::cout << obfuscated_string << std::endl;
----------------------------------------------------------------------------- */
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#ifndef AY_OBFUSCATE_DEFAULT_KEY
// The default 64 bit key to obfuscate strings with.
// This can be user specified by defining AY_OBFUSCATE_DEFAULT_KEY before
// including obfuscate.h
#define AY_OBFUSCATE_DEFAULT_KEY ay::generate_key(__LINE__)
#endif

namespace ay
{
    using size_type = unsigned long long;
    using key_type = unsigned long long;

    // Generate a psuedo-random key that spans all 8 bytes
    constexpr key_type generate_key(key_type seed)
    {
        // Use the MurmurHash3 64-bit finalizer to hash our seed
        key_type key = seed;
        key ^= (key >> 33);
        key *= 0xff51afd7ed558ccd;
        key ^= (key >> 33);
        key *= 0xc4ceb9fe1a85ec53;
        key ^= (key >> 33);

        // Make sure that a bit in each byte is set
        key |= 0x0101010101010101ull;

        return key;
    }

    // Obfuscates or deobfuscates data with key
    constexpr void cipher(char* data, size_type size, key_type key)
    {
        // Obfuscate with a simple XOR cipher based on key
        for (size_type i = 0; i < size; i++)
        {
            data[i] ^= char(key >> ((i % 8) * 8));
        }
    }

    // Obfuscates a string at compile time
    template <size_type N, key_type KEY>
    class obfuscator
    {
    public:
        // Obfuscates the string 'data' on construction
        constexpr obfuscator(const char* data)
        {
            // Copy data
            for (size_type i = 0; i < N; i++)
            {
                m_data[i] = data[i];
            }

            // On construction each of the characters in the string is
            // obfuscated with an XOR cipher based on key
            cipher(m_data, N, KEY);
        }

        constexpr const char* data() const
        {
            return &m_data[0];
        }

        constexpr size_type size() const
        {
            return N;
        }

        constexpr key_type key() const
        {
            return KEY;
        }

    private:

        char m_data[N]{};
    };

    // Handles decryption and re-encryption of an encrypted string at runtime
    //
    // 所有 obfuscated_data 实例共用一把锁。
    // 单独给每个实例配一个 mutex 会让对象不能拷贝、且体积变大，
    // 而临界区只有几十字节的 XOR，没必要。
    //
    // 关键：这把锁**故意永不析构**。
    //
    // OBFUSCATE 宏展开成「函数内 static obfuscated_data」，它的析构函数
    // 会在**库卸载**时（.fini_array / __cxa_atexit）运行，而析构里要拿这把锁。
    // 如果锁本身是个普通 static，它的析构顺序和那些 obfuscated_data 之间的
    // 先后是**不确定的**（跨翻译单元更没保证）。一旦锁先被析构，
    // 之后再去 lock 一个已析构的 mutex 就是 UB —— 在 pthread 实现上
    // 轻则死锁、重则崩溃。
    //
    // 而且现象极其难查：只在**游戏退出**时触发，表现为「退出时偶发卡死/崩溃」，
    // 和工具功能毫无关联。
    //
    // 故意泄漏一个：整个进程生命周期内有效，进程退出时由内核回收。
    // 代价是这一小块内存在进程结束前不归还 —— 可以忽略。
    inline std::mutex &obfuscateMutex()
    {
        static std::mutex *m = new std::mutex();
        return *m;
    }

    template <size_type N, key_type KEY>
    class obfuscated_data
    {
    public:
        obfuscated_data(const obfuscator<N, KEY>& obfuscator)
        {
            // Copy obfuscated data
            for (size_type i = 0; i < N; i++)
            {
                m_data[i] = obfuscator.data()[i];
            }
        }

        ~obfuscated_data()
        {
            // Zero m_data to remove it from memory.
            // 持锁：否则可能正有另一个线程在 decrypt() 里改 m_data，
            // 退出路径的清零会和它交错。
            std::lock_guard<std::mutex> guard(obfuscateMutex());
            for (size_type i = 0; i < N; i++)
            {
                m_data[i] = 0;
            }
        }

        // Returns a pointer to the plain text string, decrypting it if
        // necessary
        operator char*()
        {
            decrypt();
            return m_data;
        }

        operator std::string()
        {
            decrypt();
            return m_data;
        }

        // Manually decrypt the string
        //
        // OBFUSCATE 产生的是**全局**对象，而转换运算符会在任何线程被隐式调用
        // （渲染线程、游戏 hook 回调、后台扫描）。旧实现是裸的 check-then-act：
        // 两个线程同时看到 m_encrypted == true 就都做一次 XOR，
        // 数据被解两次 = 密文本身，返回的是乱码。
        // 而 m_data 正在被写时另一线程在读，叠加成数据竞争。
        //
        // 用一把进程级锁把「判断 + 变换」变成原子操作。
        // 临界区只有一次 XOR 的长度，冲突概率可以忽略。
        void decrypt()
        {
            if (!m_encrypted)
            {
                return;
            }
            std::lock_guard<std::mutex> guard(obfuscateMutex());
            // 拿到锁必须重新判断：等锁期间可能已被别的线程解密过
            if (m_encrypted)
            {
                cipher(m_data, N, KEY);
                m_encrypted = false;
            }
        }

        // Manually re-encrypt the string
        void encrypt()
        {
            std::lock_guard<std::mutex> guard(obfuscateMutex());
            if (!m_encrypted)
            {
                cipher(m_data, N, KEY);
                m_encrypted = true;
            }
        }

        // Returns true if this string is currently encrypted, false otherwise.
        bool is_encrypted() const
        {
            return m_encrypted;
        }

    private:

        // Local storage for the string. Call is_encrypted() to check whether or
        // not the string is currently obfuscated.
        char m_data[N];

        // Whether data is currently encrypted.
        // 只做「已解密就跳过」这个快速路径的判断；真正的状态转换在锁内。
        std::atomic<bool> m_encrypted{true};
    };

    // This function exists purely to extract the number of elements 'N' in the
    // array 'data'
    template <size_type N, key_type KEY = AY_OBFUSCATE_DEFAULT_KEY>
    constexpr auto make_obfuscator(const char(&data)[N])
    {
        return obfuscator<N, KEY>(data);
    }
}

// Obfuscates the string 'data' at compile-time and returns a reference to a
// ay::obfuscated_data object with global lifetime that has functions for
// decrypting the string and is also implicitly convertable to a char*
#define OBFUSCATE(data) OBFUSCATE_KEY(data, AY_OBFUSCATE_DEFAULT_KEY)

// Obfuscates the string 'data' with 'key' at compile-time and returns a
// reference to a ay::obfuscated_data object with global lifetime that has
// functions for decrypting the string and is also implicitly convertable to a
// char*
#define OBFUSCATE_KEY(data, key) \
	[]() -> ay::obfuscated_data<sizeof(data)/sizeof(data[0]), key>& { \
		static_assert(sizeof(decltype(key)) == sizeof(ay::key_type), "key must be a 64 bit unsigned integer"); \
		static_assert((key) >= (1ull << 56), "key must span all 8 bytes"); \
		constexpr auto n = sizeof(data)/sizeof(data[0]); \
		constexpr auto obfuscator = ay::make_obfuscator<n, key>(data); \
		static auto obfuscated_data = ay::obfuscated_data<n, key>(obfuscator); \
		return obfuscated_data; \
	}()

/* -------------------------------- LICENSE ------------------------------------
Public Domain (http://www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
----------------------------------------------------------------------------- */