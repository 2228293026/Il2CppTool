//
// Created by Perfare on 2020/7/4.
//

#include "Il2cpp.h"
#include <chrono>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
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

void il2cpp_dump(const char *outDir, const std::function<void(const char *, int, int)> &progress)
{
    LOGI("dumping...");
    size_t size;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    std::stringstream imageOutput;
    // for (int i = 0; i < size; ++i)
    // {
    //     auto image = il2cpp_assembly_get_image(assemblies[i]);
    //     imageOutput << "// Image " << i << ": " << il2cpp_image_get_name(image) << "\n";
    // }
    std::vector<std::string> outPuts;
    if (il2cpp_image_get_class)
    {
        LOGI("Version greater than 2018.3");
        // 使用il2cpp_image_get_class
        for (int i = 0; i < size; ++i)
        {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            auto imageName = il2cpp_image_get_name(image);
            progress(imageName, i, size);
            imageStr << "\n// " << imageName << "\n";
            auto classCount = il2cpp_image_get_class_count(image);
            for (int j = 0; j < classCount; ++j)
            {
                auto klass = il2cpp_image_get_class(image, j);
                auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
                // LOGD("type name : %s", il2cpp_type_get_name(type));
                auto outPut = imageStr.str() + dump_type(type);
                outPuts.push_back(outPut);
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
            LOGI("miss Assembly::Load");
            return;
        }
        if (assemblyGetTypes && assemblyGetTypes->methodPointer)
        {
            LOGI("Assembly::GetTypes: %p", assemblyGetTypes->methodPointer);
        }
        else
        {
            LOGI("miss Assembly::GetTypes");
            return;
        }
        typedef void *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
        // typedef _Il2CppArray *(*Assembly_GetTypes_ftn)(void *, void *);
        using Assembly_GetTypes_ftn = Il2CppArray<void *> *(*)(void *, void *);
        for (int i = 0; i < size; ++i)
        {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            auto image_name = il2cpp_image_get_name(image);
            imageStr << "\n// " << image_name;
            // LOGD("image name : %s", image->name);
            auto imageName = std::string(image_name);
            auto pos = imageName.rfind('.');
            auto imageNameNoExt = imageName.substr(0, pos);
            auto assemblyFileName = il2cpp_string_new(imageNameNoExt.data());
            auto reflectionAssembly =
                ((Assembly_Load_ftn)assemblyLoad->methodPointer)(nullptr, assemblyFileName, nullptr);
            auto reflectionTypes =
                ((Assembly_GetTypes_ftn)assemblyGetTypes->methodPointer)(reflectionAssembly, nullptr);
            auto items = reflectionTypes->data;
            for (int j = 0; j < reflectionTypes->max_length; ++j)
            {
                auto klass = il2cpp_class_from_system_type((Il2CppReflectionType *)items[j]);
                auto type = il2cpp_class_get_type(klass);
                // LOGD("type name : %s", il2cpp_type_get_name(type));
                auto outPut = imageStr.str() + dump_type(type);
                outPuts.push_back(outPut);
            }
        }
    }
    LOGI("write dump file");
    // auto outPath = std::string(outDir).append("/dump.cs");
    auto outPath = std::string(outDir);
    std::ofstream outStream(outPath);
    outStream << imageOutput.str();
    auto count = outPuts.size();
    for (int i = 0; i < count; ++i)
    {
        outStream << outPuts[i];
    }
    outStream.close();
    LOGI("dump done! %s", outPath.c_str());
}

void il2cpp_api_init(void *handle)
{
    LOGI("il2cpp_handle: %p", handle);
    init_il2cpp_api(handle);
    if (il2cpp_domain_get_assemblies)
    {
        Dl_info dlInfo;
        if (dladdr((void *)il2cpp_domain_get_assemblies, &dlInfo))
        {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
        LOGI("il2cpp_base: %" PRIx64 "", il2cpp_base);
    }
    else
    {
        LOGE("Failed to initialize il2cpp api.");
        return;
    }
    while (!il2cpp_is_vm_thread(nullptr))
    {
        LOGI("Waiting for il2cpp_init...");
        sleep(1);
    }
    //    auto domain = il2cpp_domain_get();
    //    il2cpp_thread_attach(domain);
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
    const std::regex pattern("(20\\d{2}|\\d)\\.(\\d)\\.(\\d{1,2})(?:[abcfp]|rc){0,2}\\d?");

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
    void Init()
    {
        auto handle = xdl_open("libil2cpp.so", 0);
        il2cpp_api_init(handle);
        xdl_close(handle);
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
        if (!curr)
        {
            LOGI("Foreign thread!");
        }
        else
        {
            LOGI("Already Attached -> %p", curr);
            return true;
        }
        LOGI("Attaching Thread");
        auto *thread = il2cpp_thread_attach(il2cpp_domain_get());
        while (!il2cpp_is_vm_thread(thread))
        {
            LOGI("Waiting...");
            sleep(1);
        }
        if (!thread)
        {
            LOGE("Attaching Failed");
            return false;
        }
        LOGI("Thread Attached");
        return true;
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
        subClassesCache.insert(std::make_pair(klass, std::make_tuple(subClasses.data(), subClasses.size())));
        return subClassesCache.at(klass);
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
        return reinterpret_cast<const char *>(il2cpp_string_chars(str));
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

    Il2CppObject *RuntimeInvokeConvertArgs(MethodInfo *method, void *obj, Il2CppObject **params, int paramCount)
    {
        return il2cpp_runtime_invoke_convert_args(method, obj, params, paramCount, nullptr);
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

    std::string getUnityVersion()
    {
        static auto Application = FindClass("UnityEngine.Application");
        static auto get_unityVersion = Application->getMethod("get_unityVersion");
        if (!get_unityVersion)
        {
            return "unknown_unity_version";
        }
        static auto unityVersion = Application->invoke_static_method<Il2CppString *>("get_unityVersion");
        if (unityVersion)
        {
            static auto str = unityVersion->to_string();
            return str;
        }
        else
        {
            LOGE("Failed to get unityVersion");
            return "unknown_unity_version";
        }
    }

    // dataPath => /storage/emulated/0/Android/data/com.dxx.firenow/files
    // identifier => com.dxx.firenow
    // version => 2.4.2
    std::string getDataPath()
    {
        static auto Application = Il2cpp::FindClass("UnityEngine.Application");
        static auto get_persistentDataPath = Application->getMethod("get_persistentDataPath");
        if (!get_persistentDataPath)
        {
            return "unknown_data_path";
        }
        static auto dataPath = get_persistentDataPath->invoke_static<Il2CppString *>("get_persistentDataPath");
        if (dataPath)
        {
            static auto str = dataPath->to_string();
            return str;
        }
        else
        {
            LOGE("Failed to get dataPath");
            return "unknown_data_path";
        }
    }
    std::string getPackageName()
    {
        static auto Application = Il2cpp::FindClass("UnityEngine.Application");
        static auto get_identifier = Application->getMethod("get_identifier");
        if (!get_identifier)
        {
            return "unknown_package_name";
        }
        static auto identifier = get_identifier->invoke_static<Il2CppString *>("get_identifier");
        if (identifier)
        {
            static auto str = identifier->to_string();
            return str;
        }

        else
        {
            LOGE("Failed to get packageName");
            return "unknown_package_name";
        }
    }

    std::string getGameVersion()
    {
        static auto Application = Il2cpp::FindClass("UnityEngine.Application");
        static auto get_version = Application->getMethod("get_version");
        if (!get_version)
        {
            return "unknown_game_version";
        }
        static auto version = get_version->invoke_static<Il2CppString *>("get_version");
        if (version)
        {
            static auto str = version->to_string();
            return str;
        }
        else
        {
            LOGE("Failed to get game version");
            return "unknown_game_version";
        }
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
            auto callback = [](Il2CppObject **arr, int size, void *userdata)
            {
                auto objects = reinterpret_cast<std::vector<Il2CppObject *> *>(userdata);
                objects->insert(objects->end(), arr, arr + size);
            };
            if (unityVersionIsBelow202120)
            {

                // typedef void (*il2cpp_WorldChangedCallback)();
                auto onWorld = []() {};

                auto state = il2cpp_unity_liveness_calculation_begin(klass, 0, callback, &objects, onWorld, onWorld);
                il2cpp_unity_liveness_calculation_from_statics(state);
                il2cpp_unity_liveness_calculation_end(state);
            }
            else
            {
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
                il2cpp_stop_gc_world();
                auto state = il2cpp_unity_liveness_allocate_struct(klass, 0, callback, &objects, realloc);
                il2cpp_unity_liveness_calculation_from_statics(state);
                il2cpp_unity_liveness_finalize(state);
                il2cpp_start_gc_world();
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
