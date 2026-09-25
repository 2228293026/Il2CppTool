#include "Frida.h"
#include "Frida/arm64-v8a/frida-gum.h"
#include "Frida/gumpp/gumpp.hpp"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Tool/Tool.h"
#include <algorithm>

// extern std::unordered_map<void *, HookerData> hookerMap;
extern int maxLine;
extern std::vector<MethodInfo *> g_Methods;

MethodInfo *binarySearchClosest(const uintptr_t addr)
{
    // g_Methods 可能为空（游戏裁剪过元数据 / 还没构建完）。
    // 旧实现是手写二分，退出时无条件 return g_Methods[right]：
    // right 为 -1（表空，或 addr 小于所有方法）就是 g_Methods[-1] 越界读。
    // 另外它把 (uintptr_t)methodPointer - addr 的结果截断进 int，64 位地址下也不可靠。
    if (g_Methods.empty())
        return nullptr;

    // g_Methods 按 methodPointer 升序。找第一个 >= addr 的位置，它的前一个就是最近的。
    auto it = std::lower_bound(
        g_Methods.begin(), g_Methods.end(), addr,
        [](const MethodInfo *m, const uintptr_t &value) { return (uintptr_t)m->methodPointer < value; });

    if (it == g_Methods.end())
        return g_Methods.back(); // addr 在所有方法之后
    if (it == g_Methods.begin())
        return *it; // addr 在所有方法之前
    return *(it - 1);
}
namespace Frida
{
    class TraceListener : public Gum::InvocationListener
    {
      private:
        Gum::RefPtr<Gum::Backtracer> backtracer;

      public:
        TraceListener() : backtracer(Gum::Backtracer_make_accurate())
        {
            if (!backtracer)
            {
                LOGE("Failed to create backtracer");
            }
        }

        void Backtracer(Gum::InvocationContext *context)
        {
            auto hookerData = context->get_listener_function_data<HookerData>();
            // hookerData->backtraced.clear();
            // hookerData->backtracing = false;
            
            {
                Gum::ReturnAddressArray return_addresses;
                backtracer->generate(context->get_cpu_context(), return_addresses);
                LOGD("========================================");
                std::vector<std::string> result;
                for (int i = 0; i < return_addresses.len; i++)
                {
                    auto addr = return_addresses.items[i];
                    // auto result =
                    //     std::min_element(g_Methods.begin(), g_Methods.end(),
                    //                      [&addr](const auto &a, const auto &b)
                    //                      {
                    //                          // Calculate absolute differences
                    //                          auto diffA = std::abs(static_cast<int>((uintptr_t)a->methodPointer -
                    //                                                                 reinterpret_cast<uintptr_t>(addr)));
                    //                          auto diffB = std::abs(static_cast<int>((uintptr_t)b->methodPointer -
                    //                                                                 reinterpret_cast<uintptr_t>(addr)));

                    //                          // Ensure addr is greater than the address
                    //                          if (addr <= a->methodPointer)
                    //                          {
                    //                              return diffA < diffB;
                    //                          }
                    //                          else
                    //                          {
                    //                              return diffB < diffA;
                    //                          }
                    //                      });
                    auto closestMethod = binarySearchClosest((uintptr_t)addr);
                    if (closestMethod)
                    {
                        intptr_t offset = (intptr_t)addr - (intptr_t)closestMethod->methodPointer;
                        if (offset < 0)
                        {
                            offset = -offset;
                        }
                        if (offset <= 0x1000)
                        {
                            char buffer[265];
                            sprintf(buffer, "%s::%s+0x%" PRIxPTR, closestMethod->getClass()->getFullName().c_str(),
                                    closestMethod->getName(), offset);
                            // LOGD("%s::%s+0x%lx => %p", closestPtr->second->getClass()->getFullName().c_str(),
                            //      closestPtr->second->getName(), gap, (void *)closestPtr->first);

                            result.push_back(buffer);
                        }
                        else
                        {
                            LOGE("Offset too big: %" PRIxPTR, offset);
                            LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                        }
                    }
                    else
                    {
                        LOGE("Not found: %p", (void *)addr);
                        LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                    }

                    // LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                }
                if (!result.empty())
                    hookerData->backtraced.push_back(result);
            }
        }

        virtual void on_enter(Gum::InvocationContext *context)
        {
            auto hookerData = context->get_listener_function_data<HookerData>();
            if (hookerData->backtracing)
            {
                Backtracer(context);
            }

            hookerData->hitCount++;
            hookerData->time = 1.f;

            if (!Il2cpp::GetIsMethodStatic(hookerData->method))
            {
                auto thiz = context->get_nth_argument<Il2CppObject *>(0);
                // int idx = 0;
                if (thiz)
                {
                    HookerData::collectSet[hookerData->method->getClass()].emplace(thiz);
                    // idx++;
                }
                // auto paramsInfo = hookerData->method->getParamsInfo();
                // for (int i = 0; i < paramsInfo.size(); i++)
                // {
                //     auto [name, type] = paramsInfo[i];
                //     auto param = context->get_nth_argument_ptr(idx);
                //     LOGD("%s %s => %p", type->getName(), name, param);
                //     if (strcmp(type->getName(), "System.String") == 0)
                //     {
                //         if (param)
                //         {
                //             auto obj = (Il2CppString *)param;
                //             auto str = obj->to_string();
                //             LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //         }
                //         else
                //         {
                //             LOGE("%s %s => null", type->getName(), name);
                //         }
                //     }
                //     else
                //     {
                //         auto ToString = type->getClass()->getMethod("ToString", 0);
                //         if (ToString)
                //         {
                //             if (type->isValueType() || type->isEnum())
                //             {
                //                 // if (type->isEnum())
                //                 // {
                //                 auto boxed = Il2cpp::GetBoxedValue(type->getClass(), &param);
                //                 auto obj = ToString->invoke_static<Il2CppString *>(boxed);
                //                 auto str = obj->to_string();
                //                 LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //                 // }
                //                 // else
                //                 // {
                //                 //     auto obj = ToString->invoke_static<Il2CppString *>(&param);
                //                 //     auto str = obj->to_string();
                //                 //     LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //                 // }
                //             }
                //             else
                //             {
                //                 auto obj = ToString->invoke_static<Il2CppString *>(param);
                //                 auto str = obj->to_string();
                //                 LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //             }
                //         }
                //         else
                //         {
                //             LOGE("%s %s => %p", type->getName(), name, param);
                //         }
                //     }
                //     idx++;
                // }
            }

            // 和 hookerHandler 保持一致：按指针去重，字符串只在首次命中时格式化。
            //
            // 顺带修掉这里的两个问题：
            // - `sprintf` 进 buffer[256]，而 %p + 类名 + 方法名长度都不受控
            //   （混淆过的 il2cpp 元数据可以很长）→ 栈溢出。
            // - 每次命中都重新格式化再逐条比较字符串。Frida 回调同样跑在
            //   游戏线程上。
            for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
            {
                if (it->address == (void *)hookerData->method->methodPointer)
                {
                    it->goneTime = 10.f;
                    it->time = 2.f;
                    it->hitCount++;
                    // std::rotate(HookerData::visited.rbegin(), it + 1, HookerData::visited.rend());
                    return;
                }
            }

            HookerTrace trace;
            trace.address = (void *)hookerData->method->methodPointer;
            trace.time = 2.f;
            trace.goneTime = 10.f;
            trace.hitCount = 0;
            char buffer[512]{0};
            const char *name = hookerData->method->getName();
            const char *className = hookerData->method->getClass() ? hookerData->method->getClass()->getName() : nullptr;
            snprintf(buffer, sizeof(buffer), "%p | %s::%s", (void *)hookerData->method->getAbsAddress(),
                     className ? className : "?", name ? name : "?");
            trace.name = buffer;
            HookerData::visited.push_back(std::move(trace));
        }

        virtual void on_leave(Gum::InvocationContext *context)
        {
        }
    };

    Gum::RefPtr<Gum::Interceptor> interceptor;
    TraceListener *traceListener;
    std::unordered_map<void *, std::unique_ptr<TraceListener>> traceListeners;
    void Init()
    {
        interceptor = Gum::Interceptor_obtain();
        traceListener = new TraceListener();
    }

    bool Trace(MethodInfo *method, HookerData *data)
    {
        auto it = traceListeners.find(data->method->methodPointer);
        if (it != traceListeners.end())
        {
            LOGE("Already hooked %s", data->method->getName());
            return false;
        }
        traceListeners[method->methodPointer] = std::make_unique<TraceListener>();
        bool result = interceptor->attach(method->methodPointer, traceListeners[method->methodPointer].get(), data);
        if (!result)
        {
            traceListeners.erase(method->methodPointer);
        }
        return result;
    }

    bool Untrace(MethodInfo *method)
    {
        auto it = traceListeners.find(method->methodPointer);
        if (it == traceListeners.end())
            return false;

        interceptor->detach(it->second.get());
        traceListeners.erase(it);
        return true;
    }

    bool isTraced(MethodInfo *method)
    {
        return traceListeners.find(method->methodPointer) != traceListeners.end();
    }
} // namespace Frida
