//
// Created by Perfare on 2020/7/4.
//

#ifndef ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H
#define ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H

#include "il2cpp-class.h"

void il2cpp_dump(const char *outDir, const std::function<void(const char *, int, int)> &progress);

namespace Il2cpp
{
    //@formatter:off

    // 返回 false 表示 il2cpp 尚未就绪（API 表没解析出来，或等待 il2cpp_init 超时）。
    // 调用方必须检查并放弃后续初始化，否则会在空指针上继续往下走。
    bool Init();
    // 区分 Init() 失败的两种原因：
    // - true  = API 表已经解析出来了，只是运行时尚未就绪 → 值得稍后重试
    // - false = 连符号都没解析出来（版本不匹配等）→ 重试也没用
    bool ApiResolved();
    // void Dump(JavaVM *jvm);
    void Dump(JNIEnv *env);
    bool EnsureAttached();
    void Detach();

    // Unity stuff
    std::string getUnityVersion();
    std::string getDataPath();
    std::string getPackageName();
    std::string getGameVersion();

    Il2CppDomain *GetDomain();
    Il2CppImage *GetImage(Il2CppAssembly *assembly);
    Il2CppImage *GetCorlib();
    Il2CppImage *GetImage(const char *assemblyName);
    Il2CppAssembly *GetAssembly(const char *name);
    Il2CppClass *GetClass(Il2CppImage *image, const char *name);
    const std::tuple<Il2CppAssembly **, size_t> &GetAssemblies();
    const std::vector<Il2CppImage *> &GetImages();

    // class
    FieldInfo *GetClassField(Il2CppClass *klass, const char *fieldName);
    FieldInfo *GetClassFields(Il2CppClass *klass, void **iter);
    MethodInfo *GetClassMethods(Il2CppClass *klass, void **iter);
    MethodInfo *GetClassMethod(Il2CppClass *klass, const char *name, int argsCount = -1);
    Il2CppImage *GetClassImage(Il2CppClass *klass);
    int32_t GetClassSize(Il2CppClass *klass);
    int32_t GetClassValueSize(Il2CppClass *klass);
    const char *GetClassName(Il2CppClass *klass);
    const char *GetClassNamespace(Il2CppClass *klass);
    std::vector<Il2CppClass *> GetClasses();
    std::vector<Il2CppClass *> GetClasses(Il2CppImage *image, const char *filter = nullptr);
    const std::tuple<Il2CppClass **, size_t> &GetSubClasses(Il2CppClass *klass);
    Il2CppType *GetClassType(Il2CppClass *klass);
    bool GetClassIsGeneric(Il2CppClass *klass);
    Il2CppClass *FindClass(const char *klassName);
    Il2CppClass *GetClassFromSystemType(Il2CppReflectionType *type);
    Il2CppType *GetBaseType(Il2CppClass *klass);
    bool GetClassIsValueType(Il2CppClass *klass);
    bool GetClassIsEnum(Il2CppClass *klass);
    bool GetClassIsStatic(Il2CppClass *klass);
    // object
    uint32_t GetObjectSize(Il2CppObject *object);
    Il2CppObject *NewObject(Il2CppClass *klass);
    Il2CppClass *GetClassParent(Il2CppClass *klass);
    Il2CppClass *GetObjectClass(Il2CppObject *object);
    bool IsClassParentOf(Il2CppClass *klass, Il2CppClass *parent);

    // image
    const char *GetImageName(Il2CppImage *image);

    // method
    uint32_t GetMethodParamCount(MethodInfo *method);
    const char *GetMethodParamName(MethodInfo *method, uint32_t index);
    const char *GetMethodName(MethodInfo *method);
    Il2CppType *GetMethodReturnType(MethodInfo *method);
    Il2CppType *GetMethodParam(MethodInfo *method, uint32_t index);
    bool GetIsMethodGeneric(MethodInfo *method);
    bool GetIsMethodInflated(MethodInfo *method);
    bool GetIsMethodStatic(MethodInfo *method);
    Il2CppReflectionMethod *GetMethodObject(MethodInfo *method, Il2CppClass *refclass = nullptr);
    MethodInfo *GetMethodFromReflection(Il2CppReflectionMethod *method);
    uint32_t GetMethodGenericCount(MethodInfo *method);
    MethodInfo *FindMethod(const char *klassName, const char *methodName, size_t argsCount = -1);
    Il2CppClass *GetMethodClass(MethodInfo *method);

    // field
    void GetFieldValue(Il2CppObject *object, FieldInfo *field, void *outValue);
    void GetFieldStaticValue(FieldInfo *field, void *outValue);
    void SetFieldValue(Il2CppObject *object, FieldInfo *field, void *newValue);
    void SetFieldStaticValue(FieldInfo *field, void *outValue);
    Il2CppObject *GetFieldValueObject(Il2CppObject *object, FieldInfo *field);
    void SetFieldValueObject(Il2CppObject *object, FieldInfo *field, Il2CppObject *newValue);
    uintptr_t GetFieldOffset(FieldInfo *field);
    Il2CppType *GetFieldType(FieldInfo *field);
    const char *GetFieldName(FieldInfo *field);
    int GetFieldFlags(FieldInfo *field);

    // type
    Il2CppClass *GetClassFromType(Il2CppType *type);
    Il2CppClass *GetTypeClass(Il2CppType *type);
    bool GetTypeIsPointer(Il2CppType *type);
    bool GetTypeIsStatic(Il2CppType *type);
    const char *GetTypeName(Il2CppType *type);
    Il2CppObject *GetTypeObject(Il2CppType *type);

    // string
    const char *GetChars(Il2CppString *str); // returns wide char
    Il2CppString *NewString(const char *str);

    // array
    uint32_t GetArrayLength(_Il2CppArray *array);
    _Il2CppArray *ArrayNew(Il2CppClass *elementTypeInfo, il2cpp_array_size_t length);
    template <typename T>
    Il2CppArray<T> *ArrayNewGeneric(Il2CppClass *elementTypeInfo, il2cpp_array_size_t length)
    {
        return static_cast<Il2CppArray<T> *>(ArrayNew(elementTypeInfo, length));
    }

    namespace GC
    {
        std::vector<Il2CppObject *> FindObjects(Il2CppClass *klass);
        void KeepAlive(Il2CppObject *object);

        // ---- 强引用（GCHandle）----
        //
        // 工具会在很多地方长期缓存托管对象（选中要画 ESP 的 GameObject、
        // 调用结果、扫描出来的实例……），但那些都是裸 Il2CppObject*。
        // 托管侧一旦没有引用者，GC 就会回收它们，之后我们手上的指针是野的 ——
        // 而且 IsNativeObjectAlive 这类检查本身也要先解引用指针才能调用，
        // 所以「先判断再用」是防不住的。
        //
        // 正确做法是给缓存的对象加一个强 GCHandle：GC 会把它当作根，
        // 只要我们不释放，对象就一直活着。get() 在对象已被回收时返回
        // nullptr（而不是野指针），可以安全地判空。
        //
        // 注意：GCHandle 是「引用计数式资源」，必须与缓存一一对应释放，
        // 否则对象永远收不掉（内存泄漏）。Release 之后再 get() 返回 null。
        uint32_t NewHandle(Il2CppObject *object, bool pinned = false);
        Il2CppObject *GetHandleTarget(uint32_t handle);
        void FreeHandle(uint32_t handle);

        // RAII 包装：析构时自动 FreeHandle。
        // 适合「函数内临时保活」这类场景。
        class ScopedHandle
        {
          public:
            ScopedHandle() = default;
            explicit ScopedHandle(Il2CppObject *object, bool pinned = false)
            {
                reset(object, pinned);
            }
            ScopedHandle(const ScopedHandle &) = delete;
            ScopedHandle &operator=(const ScopedHandle &) = delete;
            ScopedHandle(ScopedHandle &&other) noexcept
                : m_handle(other.m_handle)
            {
                other.m_handle = 0;
            }
            ScopedHandle &operator=(ScopedHandle &&other) noexcept
            {
                if (this != &other)
                {
                    release();
                    m_handle = other.m_handle;
                    other.m_handle = 0;
                }
                return *this;
            }
            ~ScopedHandle()
            {
                release();
            }

            void reset(Il2CppObject *object, bool pinned = false)
            {
                release();
                m_handle = object ? NewHandle(object, pinned) : 0;
            }
            void release()
            {
                if (m_handle)
                {
                    FreeHandle(m_handle);
                    m_handle = 0;
                }
            }
            uint32_t get() const
            {
                return m_handle;
            }
            Il2CppObject *target() const
            {
                return m_handle ? GetHandleTarget(m_handle) : nullptr;
            }
            explicit operator bool() const
            {
                return m_handle != 0;
            }

          private:
            uint32_t m_handle = 0;
        };

        // 一组被强引用的对象。
        //
        // 用于「工具会长期缓存一批对象」的场景：条目存活期间 GC 不会回收它们，
        // 条目销毁时自动释放句柄。和 ScopedHandle 一样是 move-only ——
        // 句柄只能有一个主人，浅拷贝会 double free 把 GC 句柄表写坏。
        class RootedObjectList
        {
          public:
            RootedObjectList() = default;
            explicit RootedObjectList(std::vector<Il2CppObject *> objects)
            {
                reset(std::move(objects));
            }
            RootedObjectList(const RootedObjectList &) = delete;
            RootedObjectList &operator=(const RootedObjectList &) = delete;
            RootedObjectList(RootedObjectList &&other) noexcept
            {
                swap(other);
            }
            RootedObjectList &operator=(RootedObjectList &&other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    swap(other);
                }
                return *this;
            }
            ~RootedObjectList()
            {
                reset();
            }

            // 接管一批对象并逐个加根。会先释放自己原有的句柄。
            void reset(std::vector<Il2CppObject *> objects = {})
            {
                releaseHandles();
                m_objects = std::move(objects);
                m_handles.clear();
                m_handles.reserve(m_objects.size());
                for (auto *obj : m_objects)
                {
                    m_handles.push_back(obj ? NewHandle(obj) : 0);
                }
            }

            // 丢掉所有对象和句柄
            void releaseHandles()
            {
                for (auto handle : m_handles)
                {
                    if (handle)
                    {
                        FreeHandle(handle);
                    }
                }
                m_handles.clear();
                m_objects.clear();
            }

            // 只返回仍然有效的对象。
            //
            // 关键：有效性必须由**句柄**回答，不能直接看 m_objects 里的裸指针。
            // 对象被回收后那个地址已经是野的，去解引用它就崩了。
            // 顺带把已失效的条目就地剔除（并释放其句柄），避免列表无限膨胀。
            std::vector<Il2CppObject *> liveObjects()
            {
                std::vector<Il2CppObject *> out;
                out.reserve(m_objects.size());
                for (size_t i = 0; i < m_objects.size(); i++)
                {
                    auto *obj = m_handles[i] ? GetHandleTarget(m_handles[i]) : m_objects[i];
                    if (obj == nullptr)
                    {
                        if (m_handles[i])
                        {
                            FreeHandle(m_handles[i]);
                        }
                        continue;
                    }
                    // 句柄解析出的地址可能与缓存的原始地址不同（对象被移动过），
                    // 同步更新，后续调用一律用新地址。
                    m_objects[i] = obj;
                    out.push_back(obj);
                }
                return out;
            }

            // 追加一个对象并为它加根。
            void add(Il2CppObject *object)
            {
                m_objects.push_back(object);
                m_handles.push_back(object ? NewHandle(object) : 0);
            }

            // 删除第 index 个对象并释放其句柄。
            // 必须同时删两个数组 —— 句柄和对象一一对应，错位了就等于
            // 拿 A 对象的句柄去保 B 对象（后者照样被回收）或重复释放。
            void removeAt(size_t index)
            {
                if (index >= m_objects.size())
                {
                    return;
                }
                if (m_handles[index])
                {
                    FreeHandle(m_handles[index]);
                }
                m_objects.erase(m_objects.begin() + index);
                if (index < m_handles.size())
                {
                    m_handles.erase(m_handles.begin() + index);
                }
            }

            const std::vector<Il2CppObject *> &raw() const
            {
                return m_objects;
            }
            size_t size() const
            {
                return m_objects.size();
            }
            bool empty() const
            {
                return m_objects.empty();
            }
            void swap(RootedObjectList &other) noexcept
            {
                m_objects.swap(other.m_objects);
                m_handles.swap(other.m_handles);
            }

          private:
            std::vector<Il2CppObject *> m_objects;
            // 与 m_objects 一一对应的句柄。拿不到句柄时（0）退化成裸指针并
            // 照常工作 —— 不会因此崩，只是失去了 GC 保活保证。
            std::vector<uint32_t> m_handles;
        };
    } // namespace GC

    // other
    Il2CppObject *GetBoxedValue(Il2CppClass *klass, void *value);
    void *GetUnboxedValue(Il2CppObject *object);
    template <typename T>
    T GetUnboxedValue(Il2CppObject *object)
    {
        void *value = GetUnboxedValue(object);
        return *static_cast<T *>(value);
    }
    Il2CppObject *RuntimeInvoke(MethodInfo *method, void *obj, void **params, Il2CppException **exc);
    Il2CppObject *RuntimeInvokeConvertArgs(MethodInfo *method, void *obj, Il2CppObject **params, int paramCount);
#if __DEBUG__
    // this is a Debug function, it should be used as a tool only
    void Trace(Il2CppImage *image, std::function<bool(Il2CppClass *)> filterClasses,
               std::function<bool(MethodInfo *)> filterMethods = nullptr, int maxSpam = -1);
    void Trace(Il2CppImage *image, std::initializer_list<const char *> classesFilter,
               std::initializer_list<const char *> methodsFilter, int maxSpam = -1);
#endif

    //@formatter:on
    // void il2cpp_dump(const char *outDir);
} // namespace Il2cpp

#endif // ZYGISK_IL2CPPDUMPER_IL2CPP_DUMP_H
