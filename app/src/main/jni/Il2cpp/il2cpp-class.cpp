//
// Created by misman on 02/09/23.
//

#include <algorithm>
#include "Includes/NeverDestroyedMutex.h"
#include <string>
#include <locale>
#include <codecvt>
#include <unordered_map>
#include "il2cpp-class.h"
#include "Il2cpp.h"
#include "Il2cpp/il2cpp-tabledefs.h"
#include "Tool/Util.h"
#include "sstream"

std::string toHex(uintptr_t my_integer)
{

    std::stringstream sstream;
    sstream << "0x" << std::uppercase << std::hex << my_integer;
    return sstream.str();
}

bool isVisited(std::vector<uintptr_t> &visited, Il2CppObject *object)
{
    return visited.size() > 0 && std::find(visited.begin(), visited.end(), (uintptr_t)object) != visited.end();
}

std::vector<std::function<nlohmann::ordered_json(Il2CppObject *, Il2CppType *, size_t)>> callbacks;
void AddCustomDumpHandler(std::function<nlohmann::ordered_json(Il2CppObject *, Il2CppType *, size_t)> handler)
{
    callbacks.push_back(handler);
}

nlohmann::ordered_json Handler(Il2CppObject *object, Il2CppType *type, size_t maxDepth)
{
    for (auto callback : callbacks)
    {
        auto res = callback(object, type, maxDepth);
        if (!res.empty())
        {
            return res;
        }
    }
    return nlohmann::ordered_json{};
}

int ListArraySize = 5;
bool NoCircularReference = true;

// 临时把列表/数组的展开上限调高，跑完 callback 再恢复。
//
// RAII 恢复：callback 里会做 JSON 构造、对象遍历，抛异常很常见。
// 旧代码在 callback 之后才恢复，异常一旦发生 ListArraySize 就永久停在
// 大值上，之后每次 dump 都把整个数组/列表完整展开 —— 工具会越来越卡。
void ChangeMaxListArraySize(int size, std::function<void()> callback)
{
    struct Guard
    {
        int old;
        ~Guard()
        {
            ListArraySize = old;
        }
    } guard{ListArraySize};
    ListArraySize = std::clamp(size, 5, 10000);
    callback();
}

// object may be null
nlohmann::ordered_json DumpObject(Il2CppObject *object, Il2CppType *type, std::vector<uintptr_t> &visited,
                                  size_t maxDepth)
{
    // LOGD(("DumpObject %llX %s"), object, type->getName());

    if (type->isPointer())
    {
        return "(unhandled-pointer)";
    }

    auto custom = Handler(object, type, maxDepth);
    if (!custom.empty())
    {
        return custom;
    }

    // THE ORDER OF THIS IS IMPORTANT!
    if (std::string(type->getName()) == "System.String")
    {
        auto strObj = (Il2CppString *)object;
        if (strObj)
        {
            return strObj->to_string();
        }
        else
        {
            return "(null-string)";
        }
    }
    else if (type->isPrimitive())
    {
        if (strcmp(type->getName(), "System.Int32") == 0)
        {
            return Il2cpp::GetUnboxedValue<int32_t>(object);
        }
        else if (strcmp(type->getName(), "System.Byte") == 0)
        {
            return Il2cpp::GetUnboxedValue<uint8_t>(object);
        }
        else if (strcmp(type->getName(), "System.Char") == 0)
        {
            return Il2cpp::GetUnboxedValue<wchar_t>(object);
        }
        else if (strcmp(type->getName(), "System.UInt32") == 0)
        {
            return Il2cpp::GetUnboxedValue<uint32_t>(object);
        }
        else if (strcmp(type->getName(), "System.Int64") == 0)
        {
            return Il2cpp::GetUnboxedValue<int64_t>(object);
        }
        else if (strcmp(type->getName(), "System.UInt64") == 0)
        {
            return Il2cpp::GetUnboxedValue<uint64_t>(object);
        }
        else if (strcmp(type->getName(), "System.Single") == 0)
        {
            return Il2cpp::GetUnboxedValue<float>(object);
        }
        else if (strcmp(type->getName(), "System.Double") == 0)
        {
            return Il2cpp::GetUnboxedValue<double>(object);
        }
        else if (strcmp(type->getName(), "System.Boolean") == 0)
        {
            return Il2cpp::GetUnboxedValue<bool>(object);
        }
        else if (strcmp(type->getName(), "System.Int16") == 0)
        {
            return Il2cpp::GetUnboxedValue<int16_t>(object);
        }
        else if (strcmp(type->getName(), "System.UInt16") == 0)
        {
            return Il2cpp::GetUnboxedValue<uint16_t>(object);
        }
        else
        {
            LOGD("Unhandled primitive type: %s", type->getName());
            // Handle other primitive types here
            // 名字重载在方法缺失/指针为空时会返回 T{}（即 nullptr），
            // 这里旧代码直接 ->to_string() 就是空指针解引用。
            auto str = object->invoke_method<Il2CppString *>("ToString");
            return str ? str->to_string() : std::string{};
        }
    }
    else if (type->isEnum())
    {
        if (object)
        {
            auto str = object->invoke_method<Il2CppString *>("ToString");
            return str ? str->to_string() : std::string{};
        }
        else
        {
            return "(null-enum)";
        }
    }
    else if (type->isValueType())
    {
        if (object)
        {
            return object->dump(visited, maxDepth - 1);
        }
        else
        {
            return "(null-value-type)";
        }
    }
    else if (type->isObject() && !(type->isList() || type->isArray()))
    {
        if (object)
        {
            // addVisited(visited, object);
            return object->dump(visited, maxDepth - 1);
        }
        else
        {
            return "(null-object)";
        }
    }
    else if (type->isArray())
    {
        if (object)
        {
            // addVisited(visited, object);
            if (strstr(type->getName(), "[,]"))
            {
                return "(unhandled-multi-dimensional-array)";
            }
            auto arr = (Il2CppArray<ValueType<uintptr_t>> *)object;
            nlohmann::ordered_json array = nlohmann::ordered_json::array();
            auto arrLen = arr->length();
            for (int i = 0; i < std::min(arrLen, uint32_t(ListArraySize)); i++)
            {
                auto current = arr->invoke_method<Il2CppObject *>("System.Collections.IList.get_Item", i);
                if (current)
                {
                    auto currentType = Il2cpp::GetClassType(current->klass);
                    if (currentType->isPrimitive() || currentType->isEnum())
                    {
                        array.push_back(DumpObject(current, currentType, visited, maxDepth - 1));
                    }
                    else
                    {
                        auto custom = Handler(current, currentType, maxDepth);
                        if (!custom.empty())
                        {
                            array.push_back(custom);
                        }
                        else
                        {
                            // FIXME: "System.Int32[]": "(no-fields)"
                            array.push_back(current->dump(visited, maxDepth - 1));
                        }
                    }
                }
                else
                {
                    array.push_back("(null-array-item)");
                }
            }

            if (arrLen > ListArraySize)
            {
                array.push_back(std::to_string(arrLen - ListArraySize) + " more...");
            }
            return array;
        }
        else
        {
            return "(null-array)";
        }
    }
    else if (type->isList())
    {
        auto list = (List<uintptr_t> *)object;
        if (list)
        {
            // addVisited(visited, object);
            nlohmann::ordered_json arr = nlohmann::ordered_json::array();
            auto listLen = list->size();
            for (int i = 0; i < std::min(listLen, ListArraySize); i++)
            {
                auto current = list->invoke_method<Il2CppObject *>("System.Collections.IList.get_Item", i);
                if (current)
                {
                    auto currentType = Il2cpp::GetClassType(current->klass);
                    if (currentType->isPrimitive() || currentType->isEnum())
                    {
                        arr.push_back(DumpObject(current, currentType, visited, maxDepth - 1));
                    }
                    else
                    {

                        auto custom = Handler(current, currentType, maxDepth);
                        if (!custom.empty())
                        {
                            arr.push_back({{currentType->getName(), custom}});
                        }
                        else
                        {
                            arr.push_back({{currentType->getName(), current->dump(visited, maxDepth - 1)}});
                        }
                    }
                    // arr.push_back(DumpObject(current, currentType, maxDepth - 1));
                }
                else
                {
                    arr.push_back("(null-list-item)");
                }
            }
            if (listLen > ListArraySize)
            {
                arr.push_back(std::to_string(listLen - ListArraySize) + " more...");
            }
            return arr;
        }
        else
        {
            return "(null-list)";
        }
    }
    // return "(unhandled) (" + std ::string(to_hex(type->type)) + ")";
    // return "(unhandled) (" + std ::string(type->getName()) + ")";
    return "(unhandled)";
}

nlohmann::ordered_json Il2CppObject::dump(std::vector<uintptr_t> &visited, int maxDepth, bool direct)
{
    if (NoCircularReference)
    {
        if (isVisited(visited, this))
        {
            return "(circular-reference (" + toHex((uintptr_t)this) + "))";
        }
        else
        {
            visited.push_back((uintptr_t)this);
        }
    }

    if (maxDepth == 0)
        return "(" + toHex((uintptr_t)this) + ")";

    auto thisType = Il2cpp::GetClassType(this->klass);

    if (direct || (thisType->isArray() || thisType->isList() || thisType->isEnum() ||
                   std::string(thisType->getName()) == "System.String"))
        return DumpObject(this, thisType, visited, maxDepth);

    // if (thisType->isPrimitive())
    // {
    //     auto str = this->invoke_method<Il2CppString *>("ToString")->to_string();
    //     return str;
    // }
    // else if (thisType->isValueType())
    // {
    //     // auto str = this->invoke_method<Il2CppString *>("ToString")->to_string();
    //     // return str;
    //     return "(value-type)";
    // }
    // else if (std::string(thisType->getName()) == "System.String")
    // {
    //     auto str = ((Il2CppString *)this)->to_string();
    //     return str;
    // }

    auto fields = this->klass->getFields(true);
    if (fields.size() == 0)
        return "(no-fields)";

    nlohmann::ordered_json j;
    for (auto field : fields)
    {

        auto fieldType = field->getType();
        auto fieldName = field->getName();

        if (Il2cpp::GetTypeIsStatic(fieldType) || Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)
        {
            continue;
        }

        auto jsonKey = Util::extractClassNameFromTypename(fieldType->getName()) + " " + fieldName;
        auto fieldObject = Il2cpp::GetFieldValueObject(this, field);
        j[jsonKey] = DumpObject(fieldObject, fieldType, visited, maxDepth);
    }
    return j;
}

// bool isValidPtr(void *ptr)
// {
//     if constexpr (sizeof(uintptr_t) == 4)
//     {
//         return true;
//     }
//     else
//     {
//         return ((uint32_t)(uintptr_t)ptr) > 0x10000 &&
//                (uint32_t)((uintptr_t)ptr >> (4 * sizeof(uintptr_t))) != 0xFFFFFFFF;
//     }
// }

// TODO: create another function that will returns the object given a paths
std::pair<Il2CppObject *, nlohmann::ordered_json> Il2CppObject::dump(const std::vector<std::string> &paths, bool noDump)
{
    Il2CppObject *object = this;
    if (!paths.empty())
    {
        for (int i = 0; i < paths.size() && object != nullptr; i++)
        {
            const std::string &path = paths[i];
            std::istringstream iss(path);
            std::string _, val;
            iss >> _ >> val;
            auto objKlass = Il2cpp::GetObjectClass(object);
            if (objKlass == nullptr)
            {
                return {nullptr, nlohmann::ordered_json()};
            }
            // LOGD("%s | %s %s", objKlass->getFullName().c_str(), _.c_str(), val.c_str());
            auto type = Il2cpp::GetClassType(objKlass);
            if (type == nullptr)
            {
                return {nullptr, nlohmann::ordered_json()};
            }
            if (type->isArray())
            {
                auto arr = (Il2CppArray<Il2CppObject *> *)object;
                // 同下：非数字路径不能让异常逃出去。
                int index = 0;
                try
                {
                    index = std::stoi(path);
                }
                catch (const std::exception &e)
                {
                    LOGE("dump: 数组下标 \"%s\" 不是数字: %s", path.c_str(), e.what());
                    return {nullptr, nlohmann::ordered_json()};
                }
                object = arr->invoke_method<Il2CppObject *>("System.Collections.IList.get_Item", index);
            }
            else if (type->isList())
            {
                auto list = (List<Il2CppObject *> *)object;
                // std::stoi 对非数字路径抛 std::invalid_argument。路径来自
                // JSON 里的字段名，通常不会是数字，但用户可以编辑 JSON，
                // 让异常逃出去就等于把整个游戏 terminate 掉。
                int index = 0;
                try
                {
                    index = std::stoi(path);
                }
                catch (const std::exception &e)
                {
                    LOGE("dump: 列表下标 \"%s\" 不是数字: %s", path.c_str(), e.what());
                    return {nullptr, nlohmann::ordered_json()};
                }
                object = list->invoke_method<Il2CppObject *>("System.Collections.IList.get_Item", index);
            }
            else
            {
                // getField 是**单类**查找，继承来的字段返回 null。
                //
                // 而 dump() 枚举字段用的是 getFields(true)（含父类），
                // 所以继承字段确实会出现在 JSON 里；ImGuiJson 又把这些键
                // 喂回来当路径 —— 于是这里必然拿到 null，直接传给
                // il2cpp_field_get_value_object 就是空指针解引用。
                // 旧代码没有任何判空。
                auto *field = objKlass->getFieldInHierarchy(val.c_str());
                if (field == nullptr)
                {
                    LOGE("dump: 类 %s 及其父类上都找不到字段 %s",
                         objKlass->getName() ? objKlass->getName() : "?", val.c_str());
                    return {nullptr, nlohmann::ordered_json()};
                }
                object = Il2cpp::GetFieldValueObject(object, field);
            }
            // LOGPTR(object);
        }
    }
    if (!object)
    {
        return {nullptr, nlohmann::ordered_json()};
    }
    std::vector<uintptr_t> visited;

    // ListArraySize / NoCircularReference 是全局状态，这里临时改大以便
    // 展开完整对象。旧代码在函数尾部手动恢复：一旦 dump 中途抛异常
    // （JSON 分配失败、深递归），这两个全局就永远停在「展开模式」，
    // 之后每一次 dump 都会跑满整个对象图 —— 表现为工具越用越卡。
    // RAII 保证异常路径也恢复。
    struct DumpGlobalsGuard
    {
        int oldListArraySize;
        bool oldNoCircularReference;
        DumpGlobalsGuard()
            : oldListArraySize(ListArraySize), oldNoCircularReference(NoCircularReference)
        {
            ListArraySize = 100;
            NoCircularReference = false;
        }
        ~DumpGlobalsGuard()
        {
            ListArraySize = oldListArraySize;
            NoCircularReference = oldNoCircularReference;
        }
    } globalsGuard;

    nlohmann::ordered_json result;
    if (!noDump)
    {
        result = object->dump(visited, 2);
    }
    return {object, result};
}

// 元数据里的名字**可能**为 null —— getFullName() 里早就为此加过防护，
// 注释写着「我确实遇到过 typeName 为 nullptr 的情况」。
//
// 但全代码库有 40 多处 `strcmp(type->getName(), "System.Int32")` 这种写法，
// 逐个加判空既啰嗦又容易漏（漏一处就是一个空指针解引用）。
//
// 在**源头**兜住：取不到名字时返回 "?" 这个永不匹配任何真实类型名的哨兵。
// strcmp 只会得到非 0，落到「未知类型」的兜底分支；
// std::string(sentinel) 也合法，不再是 std::string(nullptr) 那种 UB。
// 判断「名字是否有效」的地方仍可用 == "?" 来区分。
static constexpr const char *kUnknownName = "?";

const char *Il2CppClass::getName()
{
    const char *name = (this != nullptr) ? Il2cpp::GetClassName(this) : nullptr;
    return name != nullptr ? name : kUnknownName;
}

std::string Il2CppClass::getFullName()
{
    auto *classType = Il2cpp::GetClassType(this);
    const char *typeName = classType ? classType->getName() : nullptr;
    // im sure i've encounter a case where `typeName` is nullptr
    if (typeName && strlen(typeName) > 0)
    {
        return typeName;
    }
    else
    {
        std::string name = this->getName() ? this->getName() : "<null-class>";
        // getNamespace() 对全局命名空间返回 nullptr。
        // 直接 `std::string_view sv = getNamespace()` 会对空指针调用
        // char_traits::length() —— 段错误。本项目自己的 dump 代码
        // （Il2cpp.cpp 里处理 namespace 的地方）就是先判空的。
        const char *ns = getNamespace();
        std::string_view namespaze = ns ? std::string_view(ns) : std::string_view();
        if (!namespaze.empty())
        {
            name.insert(0, ".");
            name.insert(0, namespaze);
        }
        return name;
    }
}

Il2CppImage *Il2CppAssembly::getImage()
{
    return Il2cpp::GetImage(this);
}

Il2CppClass *Il2CppImage::getClass(const char *name)
{
    return Il2cpp::GetClass(this, name);
}

std::vector<Il2CppClass *> Il2CppImage::getClasses(const char *filter)
{
    return Il2cpp::GetClasses(this, filter);
}

const char *Il2CppImage::getName()
{
    return Il2cpp::GetImageName(this);
}

MethodInfo *Il2CppClass::getMethod(const char *name, size_t argsCount)
{
    return Il2cpp::GetClassMethod(this, name, argsCount);
}

MethodInfo *Il2CppClass::getMethod(const char *name, std::vector<std::string> args)
{

    for (auto m : this->getMethods())
    {
        int matched = 0;
        const char *methodName = m->getName();
        if (strcmp(methodName, name) == 0)
        {
            for (int i = 0; i < args.size(); i++)
            {
                Il2CppType *arg = Il2cpp::GetMethodParam(m, i);
                if (arg)
                {
                    auto typeName = arg->getName();
                    if (strcmp(typeName, args[i].c_str()) == 0)
                    {
                        matched++;
                    }
                    else
                    {
                        LOGD("Argument at index %d didn't matched requested "
                             "argument!\n\tRequested: %s\n\tActual: "
                             "%s\nnSkipping function...",
                             i, args[i].c_str(), typeName);
                        matched = 0;
                        break;
                    }
                }
            }
        }
        if (matched == args.size())
        {
            // getImage() 和 getNamespace() 都可能返回空：前者是装配还没解析好，
            // 后者是「全局命名空间」（il2cpp 对它返回 NULL）。%s 传空指针是 UB，
            // 而这里只是调试日志，不值得为它冒崩溃风险。
            auto *image = getImage();
            const char *imageName = image ? image->getName() : nullptr;
            const char *ns = getNamespace();
            LOGD("%s - [%s] %s::%s: %p", imageName ? imageName : "?", ns ? ns : "", getName() ? getName() : "?",
                 name ? name : "?", m);
            return m;
        }
    }
    return nullptr;
}

std::vector<MethodInfo *> Il2CppClass::getMethods(const char *filter, bool includeParents)
{
    std::vector<MethodInfo *> methods{};

    std::vector<Il2CppClass *> classes{};

    classes.push_back(this);

    if (includeParents)
    {
        auto parent = Il2cpp::GetClassParent(this);
        while (parent)
        {
            classes.push_back(parent);
            parent = Il2cpp::GetClassParent(parent);
        }
    }

    std::reverse(classes.begin(), classes.end());

    for (auto klass : classes)
    {
        void *iter = nullptr;
        while (auto method = Il2cpp::GetClassMethods(klass, &iter))
        {
            if (filter != nullptr && strstr(method->getName(), filter) == nullptr)
            {
                continue;
            }
            methods.push_back(method);
        }
    }
    return methods;
}

const char *MethodInfo::getName()
{
    // this 也可能是空（元数据被裁剪/解析失败）。
    const char *name = (this != nullptr) ? Il2cpp::GetMethodName(this) : nullptr;
    return name != nullptr ? name : kUnknownName;
}

Il2CppType *MethodInfo::getReturnType()
{
    return Il2cpp::GetMethodReturnType(this);
}

// 记录「被 hook 过的方法 → 原函数地址」。
// 这张表会被游戏线程上的 hook 回调（invoke 时查原地址）和 UI 线程
// （装/卸 hook）同时访问，没有任何同步 —— unordered_map 并发 rehash
// 就是堆破坏。
static NeverDestroyedMutex g_hookedMutex;
std::unordered_map<uintptr_t, intptr_t> alreadyHooked{};

bool MethodInfo::_isAlreadyHooked(uintptr_t ptr)
{
    std::lock_guard<NeverDestroyedMutex> guard(g_hookedMutex);
    return alreadyHooked.find(ptr) != alreadyHooked.end();
}

void MethodInfo::_addToHookedMap(uintptr_t ptr, uintptr_t oPtr)
{
    std::lock_guard<NeverDestroyedMutex> guard(g_hookedMutex);
    alreadyHooked[ptr] = oPtr;
}

// 卸 hook 时必须把登记摘掉：只 DobbyDestroy 而留着这条记录的话，
// 之后重新 hook 同一个方法会被 _isAlreadyHooked 挡掉并返回 nullptr，
// 而 invoke 依然按旧记录跳向已经失效的 trampoline。
void MethodInfo::_removeFromHookedMap(uintptr_t ptr)
{
    std::lock_guard<NeverDestroyedMutex> guard(g_hookedMutex);
    alreadyHooked.erase(ptr);
}

std::vector<std::pair<const char *, Il2CppType *>> MethodInfo::getParamsInfo()
{
    auto count = Il2cpp::GetMethodParamCount(this);
    std::vector<std::pair<const char *, Il2CppType *>> params{};
    for (int i = 0; i < count; i++)
    {
        params.emplace_back(Il2cpp::GetMethodParamName(this, i), Il2cpp::GetMethodParam(this, i));
    }
    return params;
}

MethodInfo *MethodInfo::inflate(std::initializer_list<Il2CppClass *> types)
{
    if (types.size() != Il2cpp::GetMethodGenericCount(this))
    {
        LOGE("Types generic count doesn't match");
        return nullptr;
    }
    // 解析 System.Type 所在的 corlib。
    //
    // 旧代码：
    //     static auto corlib = Il2cpp::GetCorlib();
    //     static auto systemType = corlib->getClass("System.Type");
    // 两处问题：
    //  1) corlib 没判空就解引用 —— GetCorlib() 失败（初始化太早 / 元数据
    //     被裁剪）就是**必崩**的空指针解引用。
    //  2) static 局部变量意味着「第一次拿到的 null 会永久缓存」。
    //     于是「早了一点」会变成**整个会话调用泛型方法都崩**。
    //
    // 改成每次重试：代价只是一次类查找（内部有缓存），换来能自动恢复。
    static Il2CppClass *systemType = nullptr;
    if (systemType == nullptr)
    {
        auto *corlib = Il2cpp::GetCorlib();
        if (corlib == nullptr)
        {
            LOGE("inflate: 拿不到 corlib，无法解析 System.Type");
            return nullptr;
        }
        systemType = corlib->getClass("System.Type");
        if (systemType == nullptr)
        {
            LOGE("inflate: corlib 里找不到 System.Type");
            return nullptr;
        }
    }
    auto array = Il2cpp::ArrayNewGeneric<Il2CppObject *>(systemType, types.size());
    if (array == nullptr)
    {
        LOGE("inflate: 分配 Type 数组失败（长度 %zu）", types.size());
        return nullptr;
    }
    int i = 0;
    for (auto type : types)
    {
        auto typeObj = Il2cpp::GetTypeObject(Il2cpp::GetClassType(type));
        array->data[i] = typeObj;
        i++;
    }
    auto methodObj = this->getObject();
    auto result = methodObj->invoke_method<Il2CppReflectionMethod *>("MakeGenericMethod", array);
    return Il2cpp::GetMethodFromReflection(result);
}

Il2CppClass *Il2CppClass::inflate(std::initializer_list<Il2CppClass *> types)
{
    // TODO: check generic count
    // if (types.size() != Il2cpp::GetMethodGenericCount(this))
    // {
    //     LOGE("Types generic count doesn't match");
    //     return nullptr;
    // }
    // 同上：不判空 + static 永久缓存，是同一个坑。
    static Il2CppClass *systemType = nullptr;
    if (systemType == nullptr)
    {
        auto *corlib = Il2cpp::GetCorlib();
        if (corlib == nullptr)
        {
            LOGE("Il2CppClass::inflate: 拿不到 corlib，无法解析 System.Type");
            return nullptr;
        }
        systemType = corlib->getClass("System.Type");
        if (systemType == nullptr)
        {
            LOGE("Il2CppClass::inflate: corlib 里找不到 System.Type");
            return nullptr;
        }
    }
    auto array = Il2cpp::ArrayNewGeneric<Il2CppObject *>(systemType, types.size());
    if (array == nullptr)
    {
        LOGE("Il2CppClass::inflate: 分配 Type 数组失败（长度 %zu）", types.size());
        return nullptr;
    }
    int i = 0;
    for (auto type : types)
    {
        auto typeObj = Il2cpp::GetTypeObject(Il2cpp::GetClassType(type));
        array->data[i] = typeObj;
        i++;
    }
    // auto methodObj = this->getObject();
    auto obj = Il2cpp::GetTypeObject(Il2cpp::GetClassType(this));
    auto result = obj->invoke_method<Il2CppReflectionType *>("MakeGenericType", array);
    return Il2cpp::GetClassFromSystemType(result);
}

Il2CppObject *MethodInfo::getObject()
{
    return (Il2CppObject *)Il2cpp::GetMethodObject(this);
}

uintptr_t MethodInfo::_getHookedMap(uintptr_t ptr)
{
    // 必须持锁。这张表会被游戏线程上的 hook 回调（invoke 时查原地址）
    // 和 UI 线程（装/卸 hook）同时访问：unordered_map 在 rehash 时会
    // 重建整个桶数组，另一线程此刻正在 find() 就是遍历已释放内存。
    //
    // 这个函数是整个工具**最热**的 interop 路径 —— 所有 invoke /
    // invoke_static 模板都会先查一次表拿 trampoline。
    std::lock_guard<NeverDestroyedMutex> guard(g_hookedMutex);
    auto it = alreadyHooked.find(ptr);
    if (it != alreadyHooked.end())
    {
        return it->second;
    }
    return ptr;
}

uintptr_t Il2CppObject::_getFieldOffset(const char *name)
{
    // 沿继承链找，否则继承字段的偏移取不到。
    return Il2cpp::GetFieldOffset(this->klass->getFieldInHierarchy(name));
}

FieldInfo *Il2CppClass::getField(const char *fieldName)
{
    return Il2cpp::GetClassField(this, fieldName);
}

FieldInfo *Il2CppClass::getFieldInHierarchy(const char *fieldName)
{
    if (fieldName == nullptr)
    {
        return nullptr;
    }
    // 先在自身上找（最常见的情况，避免沿链上溯的固定开销）
    if (auto *own = Il2cpp::GetClassField(this, fieldName))
    {
        return own;
    }
    // 再逐级父类。继承深度在正常类层次里很浅（1~3 层）。
    //
    // 除了正确性之外还必须防环：il2cpp 的元数据被裁剪/造假时，
    // 父类指针有可能自指或成环，无保护的上溯就是死循环 ——
    // 而这里跑在渲染线程上。
    Il2CppClass *parent = Il2cpp::GetClassParent(this);
    int guard = 0;
    constexpr int kMaxDepth = 64; // C# 单继承链，正常远小于此
    while (parent != nullptr && guard++ < kMaxDepth)
    {
        if (auto *found = Il2cpp::GetClassField(parent, fieldName))
        {
            return found;
        }
        auto *next = Il2cpp::GetClassParent(parent);
        if (next == parent)
        {
            break; // 自指
        }
        parent = next;
    }
    if (guard >= kMaxDepth)
    {
        LOGW("getFieldInHierarchy: 继承链超过 %d 层，放弃查找 %s", kMaxDepth, fieldName);
    }
    return nullptr;
}

size_t Il2CppClass::getSize()
{
    return Il2cpp::GetClassSize(this);
}

Il2CppImage *Il2CppClass::getImage()
{
    return Il2cpp::GetClassImage(this);
}

const char *Il2CppClass::getNamespace()
{
    const char *name = (this != nullptr) ? Il2cpp::GetClassNamespace(this) : nullptr;
    return name != nullptr ? name : kUnknownName;
}

std::vector<FieldInfo *> Il2CppClass::getFields(bool includeParents)
{
    std::vector<FieldInfo *> fields{};

    std::vector<Il2CppClass *> classes{};

    classes.push_back(this);

    if (includeParents)
    {
        auto parent = Il2cpp::GetClassParent(this);
        while (parent)
        {
            classes.push_back(parent);
            parent = Il2cpp::GetClassParent(parent);
        }
    }

    std::reverse(classes.begin(), classes.end());

    for (auto klass : classes)
    {
        void *iter = nullptr;
        while (auto field = Il2cpp::GetClassFields(klass, &iter))
        {
            fields.push_back(field);
        }
    }

    // void *iter = nullptr;
    // while (auto field = Il2cpp::GetClassFields(this, &iter))
    // {
    //     fields.push_back(field);
    // }
    return fields;
}

MethodInfo *Il2CppClass::findMethod(const char *name, size_t idx)
{
    if (name == nullptr)
    {
        return nullptr;
    }
    std::vector<MethodInfo *> found{};
    for (auto m : this->getMethods())
    {
        const char *methodName = (m != nullptr) ? m->getName() : nullptr;
        if (methodName == nullptr)
        {
            continue;
        }
        // 参数顺序原来反了：`strstr(name, methodName)` 是在**搜索串里找方法名**，
        // 而 getMethods 的筛选（以及这个函数自己的意图）都是反过来 ——
        // 「方法名里包含搜索串」。例如搜 "get" 时，反过来的写法会因为
        // "get" 里没有 "get_Foo" 而**一个都匹配不上**，永远返回空。
        if (strstr(methodName, name) != nullptr)
        {
            found.push_back(m);
        }
    }

    if (found.empty())
    {
        // 旧代码这里是 `return found.back()` —— 空 vector 上调 back() 是 UB，
        // 返回的是垃圾指针，调用方直接就解引用它了。
        LOGE("findMethod: 类 %s 上没有匹配 \"%s\" 的方法", this->getName() ? this->getName() : "?", name);
        return nullptr;
    }
    if (idx >= found.size())
    {
        return found.back();
    }

    return found.at(idx);
}

bool Il2CppClass::isGeneric()
{
    return Il2cpp::GetClassIsGeneric(this);
}

Il2CppType *FieldInfo::getType()
{
    return Il2cpp::GetFieldType(this);
}

uintptr_t FieldInfo::getOffset()
{
    return Il2cpp::GetFieldOffset(this);
}

const char *FieldInfo::getName()
{
    const char *name = (this != nullptr) ? Il2cpp::GetFieldName(this) : nullptr;
    return name != nullptr ? name : kUnknownName;
}

int64_t FieldInfo::getEnumStaticValue(FieldInfo *field)
{
    if (field == nullptr)
    {
        return 0;
    }
    auto *fieldType = field->getType();
    if (fieldType == nullptr)
    {
        return 0;
    }
    // 字段是枚举类型时，它的 class 才能问出底层存储类型。
    auto *baseType = Il2cpp::GetEnumBaseType(fieldType->getClass());

    // 关键：il2cpp_field_static_get_value 写入的字节数由字段类型决定，
    // 与目标变量无关。拿 4 字节缓冲去接一个 8 字节的枚举字段，运行时就会
    // 多写 4 字节，踩坏调用者的栈帧 —— 而这个 bug 只在「目标游戏里真的
    // 存在 long 底色的枚举」时才发作，本地测不出来。
    const char *name = baseType ? Il2cpp::GetTypeName(baseType) : nullptr;
    // 注意：这里必须显式捕获 field —— 它是本函数的参数，
    // 泛型 lambda 在无捕获默认时不能用外层变量。
    auto read = [field](auto tag) -> int64_t {
        using V = decltype(tag);
        V v{};
        Il2cpp::GetFieldStaticValue(field, &v);
        return static_cast<int64_t>(v);
    };
    // uint64 超过 INT64_MAX 时用位模式承载（不丢位）。
    auto readU64 = [field]() -> int64_t {
        uint64_t v = 0;
        Il2cpp::GetFieldStaticValue(field, &v);
        return static_cast<int64_t>(v);
    };

    if (name)
    {
        if (strcmp(name, "System.Byte") == 0)
            return read(uint8_t{});
        if (strcmp(name, "System.SByte") == 0)
            return read(int8_t{});
        if (strcmp(name, "System.Int16") == 0)
            return read(int16_t{});
        if (strcmp(name, "System.UInt16") == 0 || strcmp(name, "System.Char") == 0)
            return read(uint16_t{});
        if (strcmp(name, "System.Int32") == 0)
            return read(int32_t{});
        if (strcmp(name, "System.UInt32") == 0)
            return read(uint32_t{});
        if (strcmp(name, "System.Int64") == 0)
            return read(int64_t{});
        if (strcmp(name, "System.UInt64") == 0)
            return readU64();
        if (strcmp(name, "System.Boolean") == 0)
            return read(bool{}) ? 1 : 0;
    }

    // 问不到底层类型时按 8 字节读：多读的是自己栈上的临时变量，
    // 不会破坏别人的内存，比猜 4 字节安全。
    return read(int64_t{});
}

bool Il2CppType::isPointer()
{
    return Il2cpp::GetTypeIsPointer(this);
}

bool Il2CppType::isPrimitive()
{
    static std::vector<const char *> CSPrimitive = {
        "System.Boolean", "System.Char",   "System.SByte", "System.Byte",   "System.Int16",  "System.UInt16",
        "System.Int32",   "System.UInt32", "System.Int64", "System.UInt64", "System.Single", "System.Double"};
    // getName() 可能是 nullptr（getFullName() 里明确为此加了防护）。
    // 这个函数对**每个参数**都会被调到，一个空的类型就够崩一次。
    const char *typeName = this->getName();
    if (typeName == nullptr)
    {
        return false;
    }
    return std::find_if(CSPrimitive.begin(), CSPrimitive.end(),
                        [typeName](const char *primitiveName)
                        { return strcmp(typeName, primitiveName) == 0; }) != CSPrimitive.end();
}

bool Il2CppType::isValueType()
{
    // 泛型参数、指针类型、byref 类型都可能没有对应的 Il2CppClass，
    // 旧代码直接把它传给 GetClassIsValueType，那边会解引用空指针。
    // 这两个查询在 UI 的类型分派里到处被调用，一个空指针就够了。
    auto *klass = this->getClass();
    if (klass == nullptr)
    {
        return false;
    }
    return Il2cpp::GetClassIsValueType(klass);
}

bool Il2CppType::isEnum()
{
    auto *klass = this->getClass();
    if (klass == nullptr)
    {
        return false;
    }
    return Il2cpp::GetClassIsEnum(klass);
}

bool Il2CppType::isList()
{
    // getName() 可能为 nullptr，而 std::string(nullptr) 是 UB。
    const char *typeName = this->getName();
    if (typeName == nullptr)
    {
        return false;
    }
    return std::string(typeName).starts_with("System.Collections.Generic.List");
}

bool Il2CppType::isArray()
{
    switch (this->type)
    {
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
            return true;
        default:
            return false;
    }
}

bool Il2CppType::isObject()
{
    switch (this->type)
    {
        case IL2CPP_TYPE_STRING:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_GENERICINST:
            return true;
        default:
            return false;
    }
}

const char *Il2CppType::getName()
{
    const char *name = (this != nullptr) ? Il2cpp::GetTypeName(this) : nullptr;
    return name != nullptr ? name : kUnknownName;
}

Il2CppClass *Il2CppType::getClass()
{
    return Il2cpp::GetClassFromType(this);
}

std::string Il2CppString::to_string()
{
    // 必须按 il2cpp 报告的长度读，不能靠 NUL 扫描。
    //
    // 旧实现是 `std::u16string u16(reinterpret_cast<const char16_t*>(chars))`
    // —— 那个构造函数是「NUL 结尾」语义，会从 chars[0] 一路读到第一个
    // 0x0000 的字为止。但 **il2cpp 的托管字符串根本没有结尾 NUL**，
    // 长度只存在 length 字段里。扫描会一直读进托管堆，直到碰巧撞上对齐的
    // 零字才停：轻则每个字符串尾部多出一串垃圾字符（这条路径渲染了界面上的
    // 每一个字符串字段和每次调用结果），重则读过页边界 SIGSEGV。
    const char *chars = Il2cpp::GetChars(this);
    if (chars == nullptr)
    {
        return {};
    }
    const int32_t length = Il2cpp::GetStringLength(this);
    if (length <= 0)
    {
        return {};
    }
    // 显式按长度构造 u16string，不做 NUL 扫描。
    std::u16string u16(reinterpret_cast<const char16_t *>(chars),
                      reinterpret_cast<const char16_t *>(chars) + length);
    return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(u16);
}

uint32_t _Il2CppArray::length()
{
    return Il2cpp::GetArrayLength(this);
}
Il2CppObject *Il2CppClass::New()
{
    return Il2cpp::NewObject(this);
}

Il2CppClass *MethodInfo::getClass()
{
    return Il2cpp::GetMethodClass(this);
}

extern uint64_t il2cpp_base;
uintptr_t MethodInfo::getAbsAddress()
{
    return (uintptr_t)methodPointer - il2cpp_base;
}
