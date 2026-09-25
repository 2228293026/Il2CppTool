//
// Created by Perfare on 2020/7/4.
//

#include "Il2cpp.h"
#include <chrono>
#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>
#include <cinttypes>
#include <deque>
#include <jni.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <unordered_map>
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#include "xdl/include/xdl.h"

#define DO_API(r, n, p) r(*n) p

#include "il2cpp-api-functions.h"

#undef DO_API

uint64_t il2cpp_base = 0;

void init_il2cpp_api(void *handle){
#define DO_API(r, n, p)                                                                                                \
    {                                                                                                                  \
        n = (r(*) p)xdl_sym(handle, #n, nullptr);                                                                      \
    }

#include "il2cpp-api-functions.h"

#undef DO_API
}

std::string get_method_modifier(uint32_t flags)
{
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access)
    {
        case METHOD_ATTRIBUTE_PRIVATE:
            outPut << "private ";
            break;
        case METHOD_ATTRIBUTE_PUBLIC:
            outPut << "public ";
            break;
        case METHOD_ATTRIBUTE_FAMILY:
            outPut << "protected ";
            break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:
            outPut << "internal ";
            break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC)
    {
        outPut << "static ";
    }
    if (flags & METHOD_ATTRIBUTE_ABSTRACT)
    {
        outPut << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
        {
            outPut << "override ";
        }
    }
    else if (flags & METHOD_ATTRIBUTE_FINAL)
    {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
        {
            outPut << "sealed override ";
        }
    }
    else if (flags & METHOD_ATTRIBUTE_VIRTUAL)
    {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT)
        {
            outPut << "virtual ";
        }
        else
        {
            outPut << "override ";
        }
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
    {
        outPut << "extern ";
    }
    return outPut.str();
}

bool _il2cpp_type_is_byref(Il2CppType *type)
{
    auto byref = type->byref;
    if (il2cpp_type_is_byref)
    {
        byref = il2cpp_type_is_byref(type);
    }
    return byref;
}

std::string dump_method(Il2CppClass *klass)
{
    std::stringstream outPut;
    outPut << "\n\t// Methods";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter))
    {
        // TODO attribute
        // if (method->methodPointer)
        // {
        //     outPut << "\t// RVA: 0x";
        //     outPut << std::hex << (uint64_t)method->methodPointer - il2cpp_base;
        //     outPut << " VA: 0x";
        //     outPut << std::hex << (uint64_t)method->methodPointer;
        // }
        // else
        // {
        //     outPut << "\t// RVA: 0x VA: 0x0";
        // }
        /*if (method->slot != 65535) {
            outPut << " Slot: " << std::dec << method->slot;
        }*/
        outPut << "\n\t";
        uint32_t iflags = 0;
        auto flags = il2cpp_method_get_flags(method, &iflags);
        outPut << get_method_modifier(flags);
        // TODO genericContainerIndex
        auto return_type = il2cpp_method_get_return_type(method);
        if (_il2cpp_type_is_byref(return_type))
        {
            outPut << "ref ";
        }
        // auto return_class = il2cpp_class_from_type(return_type);
        // outPut << il2cpp_class_get_name(return_class) << " " << il2cpp_method_get_name(method) << "(";
        outPut << il2cpp_type_get_name(return_type) << " " << il2cpp_method_get_name(method) << "(";
        auto param_count = il2cpp_method_get_param_count(method);
        for (int i = 0; i < param_count; ++i)
        {
            auto param = il2cpp_method_get_param(method, i);
            auto attrs = param->attrs;
            if (_il2cpp_type_is_byref(param))
            {
                if (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN))
                {
                    outPut << "out ";
                }
                else if (attrs & PARAM_ATTRIBUTE_IN && !(attrs & PARAM_ATTRIBUTE_OUT))
                {
                    outPut << "in ";
                }
                else
                {
                    outPut << "ref ";
                }
            }
            else
            {
                if (attrs & PARAM_ATTRIBUTE_IN)
                {
                    outPut << "[In] ";
                }
                if (attrs & PARAM_ATTRIBUTE_OUT)
                {
                    outPut << "[Out] ";
                }
            }
            // auto parameter_class = il2cpp_class_from_type(param);
            // outPut << il2cpp_class_get_name(parameter_class) << " " << il2cpp_method_get_param_name(method, i);
            outPut << il2cpp_type_get_name(param) << " " << il2cpp_method_get_param_name(method, i);
            outPut << ", ";
        }
        if (param_count > 0)
        {
            outPut.seekp(-2, outPut.cur);
        }
        outPut << "); // ";
        if (method->methodPointer)
        {
            outPut << "0x";
            outPut << std::hex << (uint64_t)method->methodPointer - il2cpp_base;
        }
        else
        {
            outPut << "0x0";
        }
        // TODO GenericInstMethod
    }
    return outPut.str();
}

std::string dump_property(Il2CppClass *klass)
{
    std::stringstream outPut;
    outPut << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter))
    {
        // TODO attribute
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        outPut << "\t";
        Il2CppClass *prop_class = nullptr;
        uint32_t iflags = 0;
        if (get)
        {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            prop_class = il2cpp_class_from_type(il2cpp_method_get_return_type(get));
        }
        else if (set)
        {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            auto param = il2cpp_method_get_param(set, 0);
            prop_class = il2cpp_class_from_type(param);
        }
        if (prop_class)
        {
            outPut << il2cpp_class_get_name(prop_class) << " " << prop_name << " { ";
            if (get)
            {
                outPut << "get; ";
            }
            if (set)
            {
                outPut << "set; ";
            }
            outPut << "}\n";
        }
        else
        {
            if (prop_name)
            {
                outPut << " // unknown property " << prop_name;
            }
        }
    }
    return outPut.str();
}

std::string dump_field(Il2CppClass *klass)
{
    std::stringstream outPut;
    outPut << "\n\t// Fields\n";
    auto is_enum = il2cpp_class_is_enum(klass);
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter))
    {
        // TODO attribute
        outPut << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access)
        {
            case FIELD_ATTRIBUTE_PRIVATE:
                outPut << "private ";
                break;
            case FIELD_ATTRIBUTE_PUBLIC:
                outPut << "public ";
                break;
            case FIELD_ATTRIBUTE_FAMILY:
                outPut << "protected ";
                break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:
                outPut << "internal ";
                break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:
                outPut << "protected internal ";
                break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL)
        {
            outPut << "const ";
        }
        else
        {
            if (attrs & FIELD_ATTRIBUTE_STATIC)
            {
                outPut << "static ";
            }
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY)
            {
                outPut << "readonly ";
            }
        }
        auto field_type = il2cpp_field_get_type(field);
        // auto field_class = il2cpp_class_from_type(field_type);
        outPut << il2cpp_type_get_name(field_type) << " " << il2cpp_field_get_name(field);
        // TODO 获取构造函数初始化后的字段值
        if (attrs & FIELD_ATTRIBUTE_LITERAL && is_enum)
        {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        outPut << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return outPut.str();
}

std::string dump_type(Il2CppType *type)
{
    std::stringstream outPut;
    auto *klass = il2cpp_class_from_type(type);
    // outPut << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE)
    {
        outPut << "[Serializable]\n";
    }
    // TODO attribute
    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum = il2cpp_class_is_enum(klass);
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility)
    {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:
            outPut << "public ";
            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:
            outPut << "internal ";
            break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:
            outPut << "private ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:
            outPut << "protected ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED)
    {
        outPut << "static ";
    }
    else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT)
    {
        outPut << "abstract ";
    }
    else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED)
    {
        outPut << "sealed ";
    }
    if (flags & TYPE_ATTRIBUTE_INTERFACE)
    {
        outPut << "interface ";
    }
    else if (is_enum)
    {
        outPut << "enum ";
    }
    else if (is_valuetype)
    {
        outPut << "struct ";
    }
    else
    {
        outPut << "class ";
    }
    auto type_name = il2cpp_type_get_name(type);
    if (type_name && strlen(type_name) > 0)
    {
        outPut << type_name;
    }
    else
    {
        auto namespaze = il2cpp_class_get_namespace(klass);
        if (namespaze && strlen(namespaze) > 0)
        {
            outPut << namespaze << ".";
        }
        outPut << il2cpp_class_get_name(klass); // TODO genericContainerIndex
    }
    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!is_valuetype && !is_enum && parent)
    {
        auto parent_type = il2cpp_class_get_type(parent);
        // if (parent_type->type != IL2CPP_TYPE_OBJECT)
        // {
        auto name = il2cpp_type_get_name(parent_type);
        if (name && strlen(name) > 0)
        {
            extends.emplace_back(name);
        }
        else
        {
            extends.emplace_back(il2cpp_class_get_name(parent));
        }
        // }
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter))
    {
        auto type = il2cpp_class_get_type(itf);
        auto name = il2cpp_type_get_name(type);
        if (name && strlen(name) > 0)
        {
            extends.emplace_back(name);
        }
        else
        {
            extends.emplace_back(il2cpp_class_get_name(itf));
        }
    }
    outPut << " : ";
    if (!extends.empty())
    {
        outPut << extends[0];
        for (int i = 1; i < extends.size(); ++i)
        {
            outPut << ", " << extends[i];
        }
    }
    outPut << "\n{";
    outPut << dump_field(klass);
    // outPut << dump_property(klass);
    outPut << dump_method(klass);
    // TODO EventInfo
    outPut << "\n}\n";
    return outPut.str();
}

bool il2cpp_dump(const char *outDir, const std::function<bool(const char *, int, int)> &progress,
                 bool *cancelled)
{
    // 默认按「失败」处理；只有进度回调主动返回 false 时才翻成 true。
    // 旧实现两种情况都只是 return false，UI 一律显示「已取消」——
    // 磁盘满也告诉用户「你按了取消」，用户会一直重试，而不去腾空间。
    if (cancelled != nullptr)
    {
        *cancelled = false;
    }

    LOGI("dumping...");
    if (outDir == nullptr || *outDir == '\0')
    {
        LOGE("dump: 输出路径为空");
        return false;
    }
    // 这里必须检查文件是否真的打开了。
    // 旧实现 open 失败时 outStream 处于 failbit 状态，后续所有 << 都静默
    // 什么都不做，最后照样打一条 "dump done!" —— 用户看到「完成」，
    // 但磁盘上一个字节都没有。
    // 直接 trunc 打开输出文件。
    //
    // 问题是导出过程**可能被用户中途取消**（进度界面有取消按钮），而
    // 每次导出用的都是同一个文件名。于是「导到 40% 时取消」会把上一次
    // 那份**完好的** .cs 截断掉 —— 用户白白重导一次，还可能没意识到
    // 自己之前那份已经没了。
    //
    // 改成先写临时文件，成功结束时再改名替换：
    //  - 取消/失败 → 临时文件被析构删除，原文件完好无损
    //  - 成功     → rename 原子替换
    const std::string finalPath{outDir};
    const std::string tempPath{finalPath + ".part"};

    std::ofstream outStream(tempPath, std::ios::out | std::ios::trunc);
    if (!outStream.is_open())
    {
        LOGE("dump: 无法打开输出文件 %s", tempPath.c_str());
        return false;
    }

    size_t size = 0;
    auto domain = il2cpp_domain_get();
    if (domain == nullptr || il2cpp_domain_get_assemblies == nullptr)
    {
        LOGE("dump: 拿不到 il2cpp domain");
        return false;
    }
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (assemblies == nullptr || size == 0)
    {
        LOGE("dump: 没有可导出的 assembly");
        return false;
    }

    // 先把每个 image 的类数量统计出来，作为进度条的分母。
    //
    // 旧实现的进度是「每个 assembly 报一次」，而一个 assembly（尤其是
    // Assembly-CSharp）内部可能有好几千个类 —— 进度条会长时间卡在 0%，
    // 然后突然跳到 100%，用户完全不知道它在干什么。
    //
    // 而且中途想中止也没法中止。
    std::vector<size_t> classCounts(size, 0);
    size_t totalClasses = 0;
    for (size_t i = 0; i < size; ++i)
    {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (image == nullptr)
        {
            continue;
        }
        if (il2cpp_image_get_class)
        {
            classCounts[i] = (size_t)il2cpp_image_get_class_count(image);
        }
        totalClasses += classCounts[i];
    }

    size_t doneClasses = 0;
    // 进度回调返回 false 表示用户要求中止。
    auto report = [&](const char *name) -> bool
    {
        if (progress)
        {
            return progress(name, (int)doneClasses, (int)totalClasses);
        }
        return true;
    };

    if (il2cpp_image_get_class)
    {
        LOGI("Version greater than 2018.3");
        // 使用il2cpp_image_get_class
        for (size_t i = 0; i < size; ++i)
        {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            if (image == nullptr)
            {
                continue;
            }
            std::stringstream imageStr;
            auto imageName = il2cpp_image_get_name(image);
            imageStr << "\n// " << (imageName ? imageName : "?") << "\n";

            // 直接写文件，而不是攒在内存里最后再写。
            //
            // 旧实现把每个类的输出 push 进 outPuts，等全部跑完才落盘：
            // 一次 dump 的全部 .cs 内容会同时驻留在内存里（大型游戏几百 MB），
            // 而这个函数跑在后台线程、和游戏共享同一个进程 —— 挤占的是
            // 用户的可用内存，OOM 时游戏先死。
            outStream << imageStr.str();

            auto classCount = (int)classCounts[i];
            for (int j = 0; j < classCount; ++j)
            {
                // 每处理一个类就报一次进度并检查取消。
                doneClasses++;
                if (!report(imageName))
                {
                    LOGI("dump: 用户中止，已写入 %zu 个类", doneClasses);
                    outStream.flush();
                    return false;
                }
                auto klass = il2cpp_image_get_class(image, j);
                if (klass == nullptr)
                {
                    continue;
                }
                auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
                outStream << dump_type(type);
            }
        }
    }
    else
    {
        LOGI("Version less than 2018.3");
        // 使用反射
        auto corlib = il2cpp_get_corlib();
        auto assemblyClass = il2cpp_class_from_name(corlib, "System.Reflection", "Assembly");
        auto assemblyLoad = il2cpp_class_get_method_from_name(assemblyClass, "Load", 1);
        auto assemblyGetTypes = il2cpp_class_get_method_from_name(assemblyClass, "GetTypes", 0);
        if (assemblyLoad && assemblyLoad->methodPointer)
        {
            LOGI("Assembly::Load: %p", assemblyLoad->methodPointer);
        }
        else
        {
            LOGE("miss Assembly::Load");
            return false;
        }
        if (assemblyGetTypes && assemblyGetTypes->methodPointer)
        {
            LOGI("Assembly::GetTypes: %p", assemblyGetTypes->methodPointer);
        }
        else
        {
            LOGE("miss Assembly::GetTypes");
            return false;
        }
        typedef void *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
        using Assembly_GetTypes_ftn = Il2CppArray<void *> *(*)(void *, void *);
        for (size_t i = 0; i < size; ++i)
        {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            if (image == nullptr)
            {
                continue;
            }
            auto image_name = il2cpp_image_get_name(image);
            outStream << "\n// " << (image_name ? image_name : "?");

            std::string imageNameStr = image_name ? image_name : "";
            auto pos = imageNameStr.rfind('.');
            auto imageNameNoExt = imageNameStr.substr(0, pos);
            auto assemblyFileName = il2cpp_string_new(imageNameNoExt.data());
            auto reflectionAssembly =
                ((Assembly_Load_ftn)assemblyLoad->methodPointer)(nullptr, assemblyFileName, nullptr);
            if (reflectionAssembly == nullptr)
            {
                continue;
            }
            auto reflectionTypes =
                ((Assembly_GetTypes_ftn)assemblyGetTypes->methodPointer)(reflectionAssembly, nullptr);
            if (reflectionTypes == nullptr)
            {
                continue;
            }
            auto items = reflectionTypes->data;
            for (int j = 0; j < reflectionTypes->max_length; ++j)
            {
                doneClasses++;
                if (!report(image_name))
                {
                    LOGI("dump: 用户中止，已写入 %zu 个类", doneClasses);
                    outStream.flush();
                    // 这条路径是**唯一**的「用户主动取消」。
                    if (cancelled != nullptr)
                    {
                        *cancelled = true;
                    }
                    return false;
                }
                auto klass = il2cpp_class_from_system_type((Il2CppReflectionType *)items[j]);
                if (klass == nullptr)
                {
                    continue;
                }
                auto type = il2cpp_class_get_type(klass);
                outStream << dump_type(type);
            }
        }
    }

    outStream.flush();
    if (!outStream.good())
    {
        LOGE("dump: 写入 %s 时出错（存储空间不足?）", tempPath.c_str());
        return false;
    }
    outStream.close();

    // 只有确认写完了，才把临时文件换成正式文件。
    // 在此之前 cancel/失败/磁盘满，原来的 .cs 都还是完好的。
    std::error_code ec;
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec)
    {
        // 目标已存在时 POSIX rename 是覆盖语义，不该失败；真失败就退回
        // 「拷贝 + 删除」，再不行就报错。
        std::filesystem::remove(finalPath, ec);
        ec.clear();
        std::filesystem::rename(tempPath, finalPath, ec);
        if (ec)
        {
            LOGE("dump: 无法把 %s 改名为 %s: %s", tempPath.c_str(), finalPath.c_str(),
                 ec.message().c_str());
            return false;
        }
    }
    LOGI("dump done! %s (%zu 个类)", finalPath.c_str(), doneClasses);
    return true;
}

bool il2cpp_api_init(void *handle)
{
    LOGI("il2cpp_handle: %p", handle);
    init_il2cpp_api(handle);
    if (!il2cpp_domain_get_assemblies)
    {
        LOGE("Failed to initialize il2cpp api.");
        return false;
    }
    {
        Dl_info dlInfo;
        if (dladdr((void *)il2cpp_domain_get_assemblies, &dlInfo))
        {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
        LOGI("il2cpp_base: %" PRIx64 "", il2cpp_base);
    }

    // 等游戏自己完成 il2cpp_init。必须有界：这条路径最终是在
    // eglSwapBuffers 钩子里（即游戏的渲染线程）同步跑下来的，
    // 原来的 while 死等一旦判断不成立就是游戏永久卡死 + ANR。
    // 正常情况下第一次判断就该通过（能进 eglSwapBuffers 说明 il2cpp 已经起来了）。
    //
    // 还要先确认 il2cpp_is_vm_thread 这个符号真的解析出来了：
    // init_il2cpp_api 对 xdl_sym 返回空是静默忽略的，Unity 版本对不上时
    // 它可能是空函数指针，直接调用就是跳进 0 地址。
    if (!il2cpp_is_vm_thread)
    {
        LOGE("il2cpp_is_vm_thread 符号缺失（Unity 版本不匹配），放弃初始化");
        return false;
    }
    // 等游戏自己完成 il2cpp_init。
    //
    // 这里只探测一次，不 sleep：整条链最终是在 eglSwapBuffers 钩子里（游戏的渲染线程）
    // 同步跑下来的，任何 sleep 都是实打实的画面卡顿。旧代码在这里 while+sleep(1)
    // 死等 10 秒，只要渲染线程不是 VM 线程就是一次 10 秒冻结（外加未定义行为：
    // 未挂载的线程上调 il2cpp）。
    //
    // 没就绪就返回 false：调用方 on_init 会把状态置成 INIT_PENDING，
    // 由 setupMenu 在后续帧重试（游戏照常出画面），所以不需要在这里等。
    if (!il2cpp_is_vm_thread(nullptr))
    {
        LOGI("渲染线程尚未成为 il2cpp VM 线程，稍后重试");
        return false;
    }
    //    auto domain = il2cpp_domain_get();
    //    il2cpp_thread_attach(domain);
    return true;
}

bool g_DoLog = true;
class PauseLog
{
  public:
    PauseLog()
    {
        g_DoLog = false;
    }
    ~PauseLog()
    {
        g_DoLog = true;
    }
};
#define PAUSE_LOG PauseLog _

std::string repeatString(const std::string &str, int n)
{
    std::string result;
    result.reserve(str.length() * n);

    for (int i = 0; i < n; ++i)
    {
        result.append(str);
    }

    return result;
}

#include <iostream>
#include <regex>

namespace UnityVersion
{

    int compare(const std::string &a, const std::string &b);
    // 兼容 Unity 5/6：版本号可能形如 "6000.3.16f1"（Unity 6 不再是 20xx 年号），
    // 所以主号用 \d{1,4} 而不是只认 (20\d{2}|\d)。
    const std::regex pattern(R"((20\d{2}|\d{1,4})\.(\d{1,2})\.(\d{1,3})(?:[abcfp]|rc){0,2}\d?)");

    std::string find(const std::string &str)
    {
        std::smatch match;
        std::regex_search(str, match, pattern);
        return match.empty() ? "" : match[0].str();
    }

    bool gte(const std::string &a, const std::string &b)
    {
        return compare(a, b) >= 0;
    }

    bool lt(const std::string &a, const std::string &b)
    {
        return compare(a, b) < 0;
    }

    int compare(const std::string &a, const std::string &b)
    {
        std::smatch aMatches, bMatches;

        std::regex_search(a, aMatches, pattern);
        std::regex_search(b, bMatches, pattern);

        // 解析不到版本号时不要 stoi("")：按"不旧于 2021.2"处理（走新版 API 分支）
        if (aMatches.empty() || bMatches.empty())
            return 0;

        for (int i = 1; i <= 3; ++i)
        {
            int aValue = std::stoi(aMatches[i].str());
            int bValue = std::stoi(bMatches[i].str());

            if (aValue > bValue)
                return 1;
            else if (aValue < bValue)
                return -1;
        }

        return 0;
    }
} // namespace UnityVersion

namespace Il2cpp
{
    bool Init()
    {
        auto handle = xdl_open("libil2cpp.so", 0);
        bool ok = il2cpp_api_init(handle);
        xdl_close(handle);
        return ok;
    }

    bool ApiResolved()
    {
        // init_il2cpp_api 对 xdl_sym 返回空是静默忽略的，所以这里用几个关键
        // 符号是否解析出来，判断「符号没找到（版本不匹配，重试无用）」还是
        // 「运行时尚未就绪（值得稍后重试）」。
        // 必须包含 il2cpp_is_vm_thread —— il2cpp_api_init 缺它就直接判失败，
        // 漏掉它会把版本不匹配误判成「还没就绪」，白白重试 3600 帧。
        return il2cpp_domain_get_assemblies != nullptr && il2cpp_class_from_name != nullptr &&
               il2cpp_thread_current != nullptr && il2cpp_is_vm_thread != nullptr;
    }

    void Dump(JNIEnv *env)
    {
        jclass clazz = env->FindClass("com/android/support/Preferences");
        if (!clazz)
        {
            LOGE("Failed to find class: com/android/support/Preferences");
            return;
        }

        jfieldID contextField = env->GetStaticFieldID(clazz, "context", "Landroid/content/Context;");
        if (!contextField)
        {
            LOGE("Failed to find field: context");
            env->DeleteLocalRef(clazz);
            return;
        }

        jobject context = env->GetStaticObjectField(clazz, contextField);
        if (!context)
        {
            LOGE("Failed to get context");
            env->DeleteLocalRef(clazz);
            return;
        }

        jclass contextClass = env->GetObjectClass(context);
        jmethodID getExternalFilesDir =
            env->GetMethodID(contextClass, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
        if (!getExternalFilesDir)
        {
            LOGE("Failed to get method: getExternalFilesDir");
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(context);
            env->DeleteLocalRef(clazz);
            return;
        }

        jstring type = env->NewStringUTF("");
        jobject file = env->CallObjectMethod(context, getExternalFilesDir, type);
        if (!file)
        {
            LOGE("Failed to get external files directory");
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(context);
            env->DeleteLocalRef(clazz);
            return;
        }

        jclass fileClass = env->GetObjectClass(file);
        jmethodID getAbsolutePath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
        if (!getAbsolutePath)
        {
            LOGE("Failed to get method: getAbsolutePath");
            env->DeleteLocalRef(file);
            env->DeleteLocalRef(type);
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(context);
            env->DeleteLocalRef(clazz);
            return;
        }

        jstring path = (jstring)env->CallObjectMethod(file, getAbsolutePath);
        const char *str = env->GetStringUTFChars(path, nullptr);
        // il2cpp_dump(str,);
        env->ReleaseStringUTFChars(path, str);
        env->DeleteLocalRef(path);
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(type);
        env->DeleteLocalRef(contextClass);
        env->DeleteLocalRef(context);
        env->DeleteLocalRef(clazz);
    }

    bool EnsureAttached()
    {
        auto curr = il2cpp_thread_current();
        if (curr)
        {
            LOGI("Already Attached -> %p", curr);
            return true;
        }
        LOGI("Foreign thread! Attaching");
        auto *thread = il2cpp_thread_attach(il2cpp_domain_get());
        if (!thread)
        {
            LOGE("Attaching Failed");
            return false;
        }
        // 有界等待：attach 之后 VM 线程标志需要一小段时间才可见。
        // 旧代码在这里 while 死等，而且把 !thread 检查放在循环之后，
        // 一旦标志迟迟不置位就会把调用线程永久挂死；后台扫描线程调这个尤其危险。
        constexpr int kMaxWaitMs = 2000;
        for (int waited = 0; waited < kMaxWaitMs; waited += 10)
        {
            if (il2cpp_is_vm_thread(thread))
            {
                LOGI("Thread Attached");
                return true;
            }
            usleep(10 * 1000);
        }
        LOGE("Attaching Timed Out after %dms", kMaxWaitMs);
        return false;
    }

    void Detach()
    {
        auto curr = il2cpp_thread_current();
        if (!curr)
        {
            LOGI("Foreign thread!");
            return;
        }
        LOGI("Detaching Thread");
        il2cpp_thread_detach(curr);
        LOGI("Thread Detached");
    }

    Il2CppDomain *GetDomain()
    {
        return il2cpp_domain_get();
    }

    Il2CppAssembly *GetAssembly(const char *name)
    {
        auto result = il2cpp_domain_assembly_open(il2cpp_domain_get(), name);
        if (!result && g_DoLog)
            LOGE("There's no assembly : %s", name);
        return result;
    }

    Il2CppImage *GetImage(Il2CppAssembly *assembly)
    {
        // assembly 为空（模块名不对 / 元数据没就绪）时直接返回，
        // 否则 il2cpp_assembly_get_image 会拿 0 当 this 解引用。
        if (!assembly)
        {
            if (g_DoLog)
                LOGE("GetImage: assembly 为空");
            return nullptr;
        }
        auto result = il2cpp_assembly_get_image(assembly);
        if (!result && g_DoLog)
            LOGE("GetImage return nullptr");
        return result;
    }

    Il2CppImage *GetCorlib()
    {
        return il2cpp_get_corlib();
    }

    Il2CppImage *GetImage(const char *assemblyName)
    {
        return GetImage(GetAssembly(assemblyName));
    }

    Il2CppClass *GetClass(Il2CppImage *image, const char *name)
    {
        std::string nameStr = name;
        size_t dotIndex = nameStr.find_last_of('.');
        std::string classNamespace = (dotIndex == std::string::npos) ? "" : nameStr.substr(0, dotIndex);
        const std::string className = nameStr.substr(dotIndex + 1);
        auto result = il2cpp_class_from_name(image, classNamespace.c_str(), className.c_str());
        if (!result)
        {
            // LOGD("Searching [%s] by iterating through [%s]...", name, image->getName());
            auto size = il2cpp_image_get_class_count(image);
            for (size_t i{0}; i < size; i++)
            {
                auto klass = il2cpp_image_get_class(image, i);
                // il2cpp_image_get_class 对越界/未解析的索引返回 NULL，
                // 而「部分解析完成的 image」在 INIT_PENDING 阶段是常态。
                // 旧代码直接 klass->getFullName() 就是空指针解引用。
                if (klass == nullptr)
                {
                    continue;
                }
                if (klass->getFullName().compare(name) == 0)
                {
                    result = klass;
                    break;
                }
            }
            if (!result && g_DoLog)
                LOGE("There's no class : %s", name);
        }
        return result;
    }

    MethodInfo *GetClassMethod(Il2CppClass *klass, const char *methodName, int argsCount)
    {
        auto result = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
        if (!result && g_DoLog)
            LOGE("There's no method : %s in %s", methodName, klass->getFullName().c_str());
        return result;
    }

    Il2CppImage *GetClassImage(Il2CppClass *klass)
    {
        return il2cpp_class_get_image(klass);
    }

    FieldInfo *GetClassField(Il2CppClass *klass, const char *fieldName)
    {
        auto result = il2cpp_class_get_field_from_name(klass, fieldName);
        if (!result && g_DoLog)
            LOGE("There's no field : %s", fieldName);
        return result;
    }

    void GetFieldValue(Il2CppObject *object, FieldInfo *field, void *outValue)
    {
        il2cpp_field_get_value(object, field, outValue);
    }

    void SetFieldValue(Il2CppObject *object, FieldInfo *field, void *newValue)
    {
        il2cpp_field_set_value(object, field, newValue);
    }

    FieldInfo *GetClassFields(Il2CppClass *klass, void **iter)
    {
        return il2cpp_class_get_fields(klass, iter);
    }

    void GetFieldStaticValue(FieldInfo *field, void *outValue)
    {
        il2cpp_field_static_get_value(field, outValue);
    }

    void SetFieldStaticValue(FieldInfo *field, void *outValue)
    {
        il2cpp_field_static_set_value(field, outValue);
    }

    Il2CppObject *GetFieldValueObject(Il2CppObject *object, FieldInfo *field)
    {
        return il2cpp_field_get_value_object(field, object);
    }

    void SetFieldValueObject(Il2CppObject *object, FieldInfo *field, Il2CppObject *newValue)
    {
        return il2cpp_field_set_value_object(object, field, newValue);
    }

    MethodInfo *GetClassMethods(Il2CppClass *klass, void **iter)
    {
        return il2cpp_class_get_methods(klass, iter);
    }

    int32_t GetClassSize(Il2CppClass *klass)
    {
        return il2cpp_class_instance_size(klass);
    }

    int32_t GetClassValueSize(Il2CppClass *klass)
    {
        return il2cpp_class_value_size(klass, NULL);
    }

    uint32_t GetObjectSize(Il2CppObject *object)
    {
        return il2cpp_object_get_size(object);
    }
    Il2CppObject *NewObject(Il2CppClass *klass)
    {
        return il2cpp_object_new(klass);
    }

    Il2CppClass *GetClassParent(Il2CppClass *klass)
    {
        return il2cpp_class_get_parent(klass);
    }

    Il2CppClass *GetObjectClass(Il2CppObject *object)
    {
        return il2cpp_object_get_class(object);
    }

    bool IsClassParentOf(Il2CppClass *klass, Il2CppClass *parent)
    {
        while (klass)
        {
            if (klass == parent)
                return true;
            klass = GetClassParent(klass);
        }
        return false;
    }

    uint32_t GetMethodParamCount(MethodInfo *method)
    {
        return il2cpp_method_get_param_count(method);
    }

    const char *GetMethodParamName(MethodInfo *method, uint32_t index)
    {
        return il2cpp_method_get_param_name(method, index);
    }

    std::vector<Il2CppClass *> GetClasses(Il2CppImage *image, const char *filter)
    {
        std::vector<Il2CppClass *> classes;
        auto size = il2cpp_image_get_class_count(image);
        for (size_t i{0}; i < size; i++)
        {
            auto klass = il2cpp_image_get_class(image, i);
            if (!filter || strstr(klass->getFullName().c_str(), filter))
                classes.push_back(klass);
        }
        return classes;
    }

    const char *GetMethodName(MethodInfo *method)
    {
        return il2cpp_method_get_name(method);
    }

    const char *GetClassName(Il2CppClass *klass)
    {
        return il2cpp_class_get_name(klass);
    }

    const char *GetClassNamespace(Il2CppClass *klass)
    {
        return il2cpp_class_get_namespace(klass);
    }

    uintptr_t GetFieldOffset(FieldInfo *field)
    {
        return il2cpp_field_get_offset(field);
    }

    void forEachClass(Il2CppClass *klass, void *classesPtr)
    {
        auto classes = *(std::vector<Il2CppClass *> *)classesPtr;
        classes.push_back(klass);
    }

    std::vector<Il2CppClass *> GetClasses()
    {
        std::vector<Il2CppClass *> classes;
        il2cpp_class_for_each(forEachClass, &classes);
        return classes;
    }

    // 缓存必须自己持有那块内存。旧实现把局部 vector 的 data() 存进缓存，
    // 函数一返回 vector 就析构，缓存里只剩野指针 —— 任何调用者一解引用就是 UAF。
    // 用 deque 兜住：deque 增长不会让已有元素的引用失效，元素本身之后也不再改动。
    static std::deque<std::vector<Il2CppClass *>> subClassesStorage;
    std::unordered_map<Il2CppClass *, std::tuple<Il2CppClass **, size_t>> subClassesCache;
    const std::tuple<Il2CppClass **, size_t> &GetSubClasses(Il2CppClass *klass)
    {
        auto it = subClassesCache.find(klass);
        if (it != subClassesCache.end())
        {
            return it->second;
        }
        std::vector<Il2CppClass *> subClasses{};
        void *iter = nullptr;
        while (auto subKlass = il2cpp_class_get_nested_types(klass, &iter))
        {
            subClasses.push_back(subKlass);
        }
        subClassesStorage.push_back(std::move(subClasses));
        auto &stored = subClassesStorage.back();
        auto [pos, inserted] = subClassesCache.emplace(
            klass, std::make_tuple(stored.data(), stored.size()));
        return pos->second;
    }

    Il2CppType *GetClassType(Il2CppClass *klass)
    {
        return il2cpp_class_get_type(klass);
    }

    bool GetClassIsGeneric(Il2CppClass *klass)
    {
        return il2cpp_class_is_generic(klass);
    }

    Il2CppClass *FindClass(const char *klassName)
    {
        PAUSE_LOG;
        auto &images = GetImages();
        for (auto image : images)
        {
            auto klass = image->getClass(klassName);
            if (klass)
                return klass;
        }
        return nullptr;
    }

    Il2CppClass *GetClassFromSystemType(Il2CppReflectionType *type)
    {
        return il2cpp_class_from_system_type(type);
    }

    Il2CppType *GetBaseType(Il2CppClass *klass)
    {
        return il2cpp_class_enum_basetype(klass);
    }

    bool GetClassIsValueType(Il2CppClass *klass)
    {
        return il2cpp_class_is_valuetype(klass);
    }

    bool GetClassIsEnum(Il2CppClass *klass)
    {
        return il2cpp_class_is_enum(klass);
    }

    bool GetClassIsStatic(Il2CppClass *klass)
    {
        auto flags = il2cpp_class_get_flags(klass);
        return flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED;
    }

    Il2CppType *GetMethodReturnType(MethodInfo *method)
    {
        return il2cpp_method_get_return_type(method);
    }

    Il2CppType *GetMethodParam(MethodInfo *method, uint32_t index)
    {
        return il2cpp_method_get_param(method, index);
    }

    bool GetIsMethodGeneric(MethodInfo *method)
    {
        return il2cpp_method_is_generic(method);
    }

    bool GetIsMethodInflated(MethodInfo *method)
    {
        return il2cpp_method_is_inflated(method);
    }

    bool GetIsMethodStatic(MethodInfo *method)
    {
        return !il2cpp_method_is_instance(method);
    }

    Il2CppReflectionMethod *GetMethodObject(MethodInfo *method, Il2CppClass *refclass)
    {
        return il2cpp_method_get_object(method, refclass);
    }

    MethodInfo *GetMethodFromReflection(Il2CppReflectionMethod *method)
    {
        return il2cpp_method_get_from_reflection(method);
    }

    uint32_t GetMethodGenericCount(MethodInfo *method)
    {
        auto obj = method->getObject();
        auto args = obj->invoke_method<Il2CppArray<Il2CppObject *> *>("GetGenericArguments");
        return args->length();
    }

    MethodInfo *FindMethod(const char *klassName, const char *methodName, size_t argsCount)
    {
        PAUSE_LOG;
        auto &images = GetImages();
        for (auto image : images)
        {
            auto klass = image->getClass(klassName);
            if (klass)
            {
                return GetClassMethod(klass, methodName, argsCount);
            }
        }
        return nullptr;
    }

    Il2CppClass *GetMethodClass(MethodInfo *method)
    {
        return il2cpp_method_get_class(method);
    }

    Il2CppClass *GetClassFromType(Il2CppType *type)
    {
        return il2cpp_class_from_type(type);
    }

    Il2CppClass *GetTypeClass(Il2CppType *type)
    {
        return il2cpp_type_get_class_or_element_class(type);
    }

    bool GetTypeIsPointer(Il2CppType *type)
    {
        return il2cpp_type_is_pointer_type(type);
    }

    bool GetTypeIsStatic(Il2CppType *type)
    {
        return il2cpp_type_is_static(type);
    }

    Il2CppType *GetFieldType(FieldInfo *field)
    {
        return il2cpp_field_get_type(field);
    }

    // 枚举的底层存储类型。C# enum 的底层可以是 byte/sbyte/short/ushort/
    // int/uint/long/ulong，宽度不一；读值时必须按这个类型来，否则会用
    // 4 字节的临时变量去接 8 字节的字段，把调用者的栈写坏。
    Il2CppType *GetEnumBaseType(Il2CppClass *klass)
    {
        if (klass == nullptr || !il2cpp_class_enum_basetype)
        {
            return nullptr;
        }
        return il2cpp_class_enum_basetype(klass);
    }

    const char *GetFieldName(FieldInfo *field)
    {
        return il2cpp_field_get_name(field);
    }

    int GetFieldFlags(FieldInfo *field)
    {
        return il2cpp_field_get_flags(field);
    }

    const char *GetTypeName(Il2CppType *type)
    {
        return il2cpp_type_get_name(type);
    }

    Il2CppObject *GetTypeObject(Il2CppType *type)
    {
        return il2cpp_type_get_object(type);
    }

    const char *GetChars(Il2CppString *str)
    {
        if (str == nullptr || !il2cpp_string_chars)
        {
            return nullptr;
        }
        return reinterpret_cast<const char *>(il2cpp_string_chars(str));
    }

    // il2cpp 托管字符串的**字符数**。
    //
    // 关键：il2cpp 的字符串**不是** NUL 结尾的，长度只存在 length 字段里。
    // 想按「已知长度」读出内容就必须问 il2cpp_string_length ——
    // 靠 NUL 扫描会一路读进托管堆，直到碰巧撞上一个 0x0000 的字为止：
    // 轻则尾部多出一串垃圾字符（用户看到的每个字符串都被污染），
    // 重则读过页边界直接 SIGSEGV。
    int32_t GetStringLength(Il2CppString *str)
    {
        if (str == nullptr || !il2cpp_string_length)
        {
            return 0;
        }
        return il2cpp_string_length(str);
    }

    Il2CppString *NewString(const char *str)
    {
        return il2cpp_string_new(str);
    }

    const char *GetImageName(Il2CppImage *image)
    {
        return il2cpp_image_get_name(image);
    }

    uint32_t GetArrayLength(_Il2CppArray *array)
    {
        return il2cpp_array_length(array);
    }

    _Il2CppArray *ArrayNew(Il2CppClass *elementTypeInfo, il2cpp_array_size_t length)
    {
        return il2cpp_array_new(elementTypeInfo, length);
    }

    Il2CppObject *GetBoxedValue(Il2CppClass *klass, void *value)
    {
        return il2cpp_value_box(klass, value);
    }

    void *GetUnboxedValue(Il2CppObject *object)
    {
        return il2cpp_object_unbox(object);
    }

    Il2CppObject *RuntimeInvoke(MethodInfo *method, void *obj, void **params, Il2CppException **exc)
    {
        return il2cpp_runtime_invoke(method, obj, params, exc);
    }

    // 带异常出参的版本。
    //
    // 旧签名把 Il2CppException** 硬编码成 nullptr，于是**方法抛异常时**，
    // 返回值是 null，UI 就显示 "the call returned null" —— 把「抛异常」
    // 和「合法地返回 null」混为一谈。这个面板的卖点恰恰是
    // 「查看返回值 / 异常」，说错比不说更糟。
    //
    // 现在让调用方能拿到异常对象自己判别。
    Il2CppObject *RuntimeInvokeConvertArgs(MethodInfo *method, void *obj, Il2CppObject **params,
                                           int paramCount, Il2CppException **exception)
    {
        if (method == nullptr)
        {
            return nullptr;
        }
        return il2cpp_runtime_invoke_convert_args(method, obj, params, paramCount, exception);
    }

    std::tuple<Il2CppAssembly **, size_t> assembliesCache{nullptr, 0};
    const std::tuple<Il2CppAssembly **, size_t> &GetAssemblies()
    {
        const auto &[ass, size2] = assembliesCache;
        if (size2 > 0)
        {
            return assembliesCache;
        }
        size_t size = 0;
        auto asss = il2cpp_domain_get_assemblies(GetDomain(), &size);
        assembliesCache = std::make_tuple(asss, size);
        return assembliesCache;
    }

    std::vector<Il2CppImage *> imagesCache;
    const std::vector<Il2CppImage *> &GetImages()
    {
        if (!imagesCache.empty())
            return imagesCache;
        const auto &[ass, size] = GetAssemblies();
        for (size_t i = 0; i < size; i++)
        {
            imagesCache.push_back(GetImage(ass[i]));
        }
        return imagesCache;
    }

    // 安全地读 UnityEngine.Application 上的一个静态字符串属性。
    //
    // 旧代码是 `static auto Application = FindClass("UnityEngine.Application");
    // static auto m = Application->getMethod(...)` —— 完全没判空。
    // 而 GC::FindObjects 的第一件事就是调 getUnityVersion()，跑在后台扫描
    // 线程上：只要元数据被裁剪、类名对不上、或者查取得太早（Application
    // 还没加载），FindClass 返回 null，这里就是一个必崩的空指针解引用 ——
    // 而且崩在后台线程上，用户只看到工具「没反应」+ 游戏 ANR。
    static std::string getApplicationString(const char *property, const char *fallback)
    {
        // 缓存结果：这些值在进程生命周期内不变。
        // 注意 fallback 也要缓存 —— 否则每次失败都打一条日志，
        // 扫描线程每秒调几次会把日志刷爆。
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, std::string> cache;

        {
            std::lock_guard<std::mutex> guard(cacheMutex);
            auto it = cache.find(property);
            if (it != cache.end())
            {
                return it->second;
            }
        }

        std::string result = fallback;
        auto *Application = FindClass("UnityEngine.Application");
        if (Application == nullptr)
        {
            LOGE("找不到 UnityEngine.Application（%s 退化为 %s）", property, fallback);
        }
        else
        {
            auto *method = Application->getMethod(property);
            if (method == nullptr)
            {
                LOGE("UnityEngine.Application 没有 %s", property);
            }
            else
            {
                try
                {
                    auto *value = method->invoke_static<Il2CppString *>(property);
                    if (value)
                    {
                        result = value->to_string();
                    }
                    else
                    {
                        LOGE("读取 %s 返回空", property);
                    }
                }
                catch (const std::exception &e)
                {
                    LOGE("读取 %s 抛出异常: %s", property, e.what());
                }
                catch (...)
                {
                    LOGE("读取 %s 抛出未知异常", property);
                }
            }
        }

        std::lock_guard<std::mutex> guard(cacheMutex);
        cache[property] = result;
        return result;
    }

    std::string getUnityVersion()
    {
        return getApplicationString("get_unityVersion", "unknown_unity_version");
    }

    // dataPath => /storage/emulated/0/Android/data/com.dxx.firenow/files
    // identifier => com.dxx.firenow
    // version => 2.4.2
    std::string getDataPath()
    {
        return getApplicationString("get_persistentDataPath", "unknown_data_path");
    }
    std::string getPackageName()
    {
        return getApplicationString("get_identifier", "unknown_package_name");
    }

    std::string getGameVersion()
    {
        return getApplicationString("get_version", "unknown_game_version");
    }
    namespace GC
    {
        // TODO: handle other unity versions
        std::vector<Il2CppObject *> FindObjects(Il2CppClass *klass)
        {
            static auto unityVersion = getUnityVersion();
            static bool unityVersionIsBelow202120 = UnityVersion::lt(unityVersion, "2021.2.0");
            LOGD("Seaching objects for %s", klass->getFullName().c_str());

            std::vector<Il2CppObject *> objects;
            // typedef void (*il2cpp_register_object_callback)(Il2CppObject **arr, int size, void *userdata);
            // 这个回调是 il2cpp 通过 C 栈调进来的，绝不能让异常穿出去；
            // 而且下面 stop_gc_world 之后一旦有异常逃出去，游戏的 GC 就永久停住（直接冻死）。
            auto callback = [](Il2CppObject **arr, int size, void *userdata)
            {
                auto objects = reinterpret_cast<std::vector<Il2CppObject *> *>(userdata);
                try
                {
                    objects->insert(objects->end(), arr, arr + size);
                }
                catch (...)
                {
                    // 分配失败就少收一批，不能把异常抛回 il2cpp
                }
            };
            if (unityVersionIsBelow202120)
            {
                if (!il2cpp_unity_liveness_calculation_begin || !il2cpp_unity_liveness_calculation_from_statics ||
                    !il2cpp_unity_liveness_calculation_end)
                {
                    // Unity 6 等新版本可能已改名/移除这些内部符号，缺了就直接空跑，别调空指针
                    LOGE("GC::FindObjects: il2cpp_unity_liveness_* 符号缺失（Unity 版本不匹配），无法枚举对象");
                    return objects;
                }

                // typedef void (*il2cpp_WorldChangedCallback)();
                auto onWorld = []() {};

                auto state = il2cpp_unity_liveness_calculation_begin(klass, 0, callback, &objects, onWorld, onWorld);
                il2cpp_unity_liveness_calculation_from_statics(state);
                il2cpp_unity_liveness_calculation_end(state);
            }
            else
            {
                if (!il2cpp_stop_gc_world || !il2cpp_start_gc_world || !il2cpp_unity_liveness_allocate_struct ||
                    !il2cpp_unity_liveness_calculation_from_statics || !il2cpp_unity_liveness_finalize ||
                    !il2cpp_unity_liveness_free_struct)
                {
                    LOGE("GC::FindObjects: stop_gc_world/liveness 符号缺失（Unity 6 等），无法枚举对象");
                    return objects;
                }

                // typedef void *(*il2cpp_liveness_reallocate_callback)(void *ptr, size_t size, void *userdata);
                auto realloc = [](void *ptr, size_t size, void *userdata) -> void *
                {
                    if (ptr != nullptr && size == 0)
                    {
                        il2cpp_free(ptr);
                        return nullptr;
                    }
                    else
                    {
                        return il2cpp_alloc(size);
                    }
                };
                // RAII：stop_gc_world 之后必须保证 start_gc_world 一定被调用。
                // 旧代码是顺序裸调，只要中间任何一步抛异常（分配失败、vector 扩容…）
                // 就会带着「GC 已停」的状态退出，游戏的 GC 从此永久停摆 —— 直接冻死，
                // 而且不会有任何报错。
                struct GcWorldRestart
                {
                    bool stopped = false;
                    ~GcWorldRestart()
                    {
                        if (stopped)
                            il2cpp_start_gc_world();
                    }
                } worldGuard;

                il2cpp_stop_gc_world();
                worldGuard.stopped = true;
                auto state = il2cpp_unity_liveness_allocate_struct(klass, 0, callback, &objects, realloc);
                if (!state)
                {
                    LOGE("GC::FindObjects: liveness_allocate_struct 返回空");
                    return objects; // 析构时自动重启 GC world
                }
                il2cpp_unity_liveness_calculation_from_statics(state);
                il2cpp_unity_liveness_finalize(state);
                il2cpp_start_gc_world();
                worldGuard.stopped = false;
                il2cpp_unity_liveness_free_struct(state);
            }
            LOGD("Found %lu objects", objects.size());
            return objects;
        }
        // is this function actually work?
        void KeepAlive(Il2CppObject *object)
        {
            static auto SystemGC = FindClass("System.GC");
            SystemGC->invoke_static_method<void>("KeepAlive", object);
        }

        uint32_t NewHandle(Il2CppObject *object, bool pinned)
        {
            if (object == nullptr)
            {
                return 0;
            }
            if (!il2cpp_gchandle_new)
            {
                // 符号缺失（老版本 il2cpp）时不能返回 0 假装成功：
                // 调用方会以为「已加根」，实际没有，问题会延后到随机崩溃才暴露。
                LOGE("GC::NewHandle: il2cpp_gchandle_new 符号缺失，无法加根");
                return 0;
            }
            // pinned=true 会让 GC 永不移动该对象（仅在需要稳定地址时用，
            // 默认 false 避免让本就不紧凑的堆更碎片化）。
            auto handle = il2cpp_gchandle_new(object, pinned);
            if (handle == 0)
            {
                LOGE("GC::NewHandle: 分配 gchandle 失败");
            }
            return handle;
        }

        Il2CppObject *GetHandleTarget(uint32_t handle)
        {
            if (handle == 0 || !il2cpp_gchandle_get_target)
            {
                return nullptr;
            }
            // 对象若已被回收，il2cpp 返回 nullptr —— 这正是我们要的：
            // 可以安全判空，而不是拿着野指针去解引用。
            return il2cpp_gchandle_get_target(handle);
        }

        void FreeHandle(uint32_t handle)
        {
            if (handle == 0 || !il2cpp_gchandle_free)
            {
                return;
            }
            il2cpp_gchandle_free(handle);
        }
    } // namespace GC
#ifdef USE_FRIDA
    #include "gumpp.hpp"
    struct TracerData
    {
        MethodInfo *m;
        std::chrono::time_point<std::chrono::system_clock> lastTime;
        int count;
        int maxCount;
        bool skip;
    };
    class TracerLintener : public Gum::InvocationListener
    {
      public:
        TracerLintener()
        {
        }

        virtual void on_enter(Gum::InvocationContext *context)
        {
            auto data = (TracerData *)context->get_listener_function_data_ptr();
            auto m = data->m;
            if (!data->skip)
            {
                auto now = std::chrono::system_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - data->lastTime).count() > 500)
                {
                    data->count = 0;
                }
                data->lastTime = now;
                data->count++;
                if (data->maxCount > 0 && data->count > data->maxCount)
                {
                    data->skip = true;
                    return;
                }

                auto indent = repeatString("│ ", depth++);
                LOGD("%lu %s%s%s::%s", (uintptr_t)data->m->methodPointer - il2cpp_base, indent.c_str(), "┌─",
                     m->getClass()->getFullName().c_str(), m->getName());
            }
        }

        virtual void on_leave(Gum::InvocationContext *context)
        {
            auto data = (TracerData *)context->get_listener_function_data_ptr();
            if (data->skip)
                return;
            auto indent = repeatString("│ ", --depth);
            LOGD("%lu %s%s%s::%s", (uintptr_t)data->m->methodPointer - il2cpp_base, indent.c_str(), "└─",
                 data->m->getClass()->getFullName().c_str(), data->m->getName());
            // TODO: Improve this
            //  auto returnType = data->m->getReturnType()->getName();
            //  if (strcmp(returnType, "System.Int32") == 0)
            //  {
            //      int32_t *returnValue = context->get_return_value<int32_t *>();
            //      LOGD("%s%s%s::%s = %d", indent.c_str(), "└─", data->m->getClass()->getFullName().c_str(),
            //           data->m->getName(), &returnValue);
            //  }
            //  else if (strcmp(returnType, "System.Int64") == 0)
            //  {
            //      int64_t *returnValue = context->get_return_value<int64_t *>();
            //      LOGD("%s%s%s::%s = %lld", indent.c_str(), "└─", data->m->getClass()->getFullName().c_str(),
            //           data->m->getName(), &returnValue);
            //  }
            //  else if (strcmp(returnType, "System.Single") == 0)
            //  {
            //      float *returnValue = context->get_return_value<float *>();
            //      LOGD("%s%s%s::%s = %f", indent.c_str(), "└─", data->m->getClass()->getFullName().c_str(),
            //           data->m->getName(), &returnValue);
            //  }
            //  else if (strcmp(returnType, "System.Double") == 0)
            //  {
            //      double *returnValue = context->get_return_value<double *>();
            //      LOGD("%s%s%s::%s = %lf", indent.c_str(), "└─", data->m->getClass()->getFullName().c_str(),
            //           data->m->getName(), &returnValue);
            //  }
            //  else
            //  {
            //      LOGD("%s%s%s::%s", indent.c_str(), "└─", data->m->getClass()->getFullName().c_str(),
            //           data->m->getName());
            //  }
        }
        static int depth;
        static std::unordered_map<void *, TracerData *> spamCounter;
    };
    int TracerLintener::depth = 0;
    std::unordered_map<void *, TracerData *> TracerLintener::spamCounter{};
    void Trace(Il2CppImage *image, std::function<bool(Il2CppClass *)> filterClasses,
               std::function<bool(MethodInfo *)> filterMethods, int maxSpam)
    {
        auto classes = image->getClasses();
        auto traceCount = 0;
        if (classes.empty())
        {
            LOGE("Image %s has no classes", image->getName());
            return;
        }

        Gum::RefPtr<Gum::Interceptor> interceptor(Gum::Interceptor_obtain());

        TracerLintener *listener = new TracerLintener();
        for (auto klass : classes)
        {
            if (filterClasses && !filterClasses(klass))
                continue;
            for (auto m : klass->getMethods())
            {
                if (filterMethods && !filterMethods(m))
                    continue;
                auto str = klass->getFullName() + "::" + m->getName();
                if (!m->methodPointer ||
                    !interceptor->attach(m->methodPointer, listener,
                                         new TracerData{m, std::chrono::system_clock::now(), 0, maxSpam, false}))
                {
                    LOGE("Failed to instrument %s", str.c_str());
                }
                else
                {
                    traceCount++;
                    LOGD("Tracing %s", str.c_str());
                }
            }
        }
        LOGI("DONE Traced %d methods", traceCount);
    }
    void Trace(Il2CppImage *image, std::initializer_list<const char *> classesFilter,
               std::initializer_list<const char *> methodsFilter, int maxSpam)
    {
        Trace(
            image,
            [&classesFilter](Il2CppClass *klass)
            {
                if (classesFilter.size() == 0)
                    return true;
                for (auto name : classesFilter)
                {
                    if (std::strstr(klass->getFullName().c_str(), name))
                        return true;
                }
                return false;
            },
            [&methodsFilter](MethodInfo *m)
            {
                if (methodsFilter.size() == 0)
                    return true;
                for (auto name : methodsFilter)
                {
                    if (std::strstr(m->getName(), name))
                        return true;
                }
                return false;
            },
            maxSpam);
    }
#endif
} // namespace Il2cpp
