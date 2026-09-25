#include "KittyUtils.h"

// std::isxdigit 需要 <cctype>。之前只靠传递包含才编过 —— 一旦某个包含
// KittyUtils.h 的文件没间接带上 <cctype>，换个工具链就会直接编译失败。
#include <cctype>
#include "Includes/Logger.h"

static void xtrim(std::string &hex){
    if(hex.compare(0, 2, "0x") == 0){
        hex.erase(0, 2);
    }

    // https://www.techiedelight.com/remove-whitespaces-string-cpp/
    hex.erase(std::remove_if(hex.begin(), hex.end(), [](char c){
								return (c == ' ' || c == '\n' || c == '\r' ||
										c == '\t' || c == '\v' || c == '\f');
							}),
							hex.end());
}


bool KittyUtils::validateHexString(std::string &xstr){
    if(xstr.length() < 2) return false;
    xtrim(xstr); // first remove spaces
    if(xstr.length() % 2 != 0) return false;
    for(size_t i = 0; i < xstr.length(); i++){
        if(!std::isxdigit((unsigned char)xstr[i])){
            return false;
        }
    }
    return true;
}


// https://tweex.net/post/c-anything-tofrom-a-hex-string/
#include <sstream>
#include <iomanip>


// ------------------------------------------------------------------
/*!
    Convert a block of data to a hex string
*/
void KittyUtils::toHex(
    void *const data,           //!< Data to convert
    const size_t dataLength,    //!< Length of the data to convert
    std::string &dest           //!< Destination string
    )
{
    unsigned char     *byteData = reinterpret_cast<unsigned char*>(data);
    std::stringstream hexStringStream;
    
    hexStringStream << std::hex << std::setfill('0');
    for(size_t index = 0; index < dataLength; ++index)
        hexStringStream << std::setw(2) << static_cast<int>(byteData[index]);
    dest = hexStringStream.str();
}


// ------------------------------------------------------------------
/*!
    Convert a hex string to a block of data
*/
void KittyUtils::fromHex(
    const std::string &in,     //!< Input hex string
    void *const data           //!< Data store
    )
{
    size_t          length    = in.length();
    unsigned char   *byteData = reinterpret_cast<unsigned char*>(data);

    // 奇数长度会**写穿调用方的缓冲区**。
    //
    // 循环按 strIndex 每次推进 2，条件却是 strIndex < length：
    // length == 5 时循环体跑 3 次（strIndex 0→2→4→6），写 3 个字节，
    // 而调用方按 `hex.length() / 2` == 2 来分配 —— 多写 1 字节。
    //
    // 顺带的问题：没有校验非十六进制字符，"zz" 会被静默转成 0x00，
    // 用一个错误的目标字节去改游戏内存，比直接失败糟糕得多。
    if ((length & 1) != 0)
    {
        LOGE("fromHex: 十六进制串长度必须是偶数（当前 %zu）", length);
        return;
    }

    std::stringstream hexStringStream; hexStringStream >> std::hex;
    for(size_t strIndex = 0, dataIndex = 0; strIndex < length; ++dataIndex)
    {
        // Read out and convert the string two characters at a time
        const char tmpStr[3] = { in[strIndex++], in[strIndex++], 0 };

        // 非十六进制字符直接失败，别把 0x00 当成有效字节写进去。
        if (!std::isxdigit(static_cast<unsigned char>(tmpStr[0])) ||
            !std::isxdigit(static_cast<unsigned char>(tmpStr[1])))
        {
            LOGE("fromHex: \"%s\" 不是合法的十六进制串，已中止", in.c_str());
            return;
        }

        // Reset and fill the string stream
        hexStringStream.clear();
        hexStringStream.str(tmpStr);

        // Do the conversion
        int tmpValue = 0;
        hexStringStream >> tmpValue;
        byteData[dataIndex] = static_cast<unsigned char>(tmpValue);
    }
}