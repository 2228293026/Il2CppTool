#include "Frida.h"
#include "Frida/arm64-v8a/frida-gum.h"
#include "Frida/gumpp/gumpp.hpp"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Tool/Tool.h"

// extern std::unordered_map<void *, HookerData> hookerMap;
extern int maxLine;
extern std::vector<MethodInfo *> g_Methods;

MethodInfo *binarySearchClosest(const uintptr_t addr)
{
    int left = 0;
    int right = g_Methods.size() - 1;

    while (left <= right)
    {
        int pivot = (left + right) / 2;
        int comparison = (uintptr_t)g_Methods[pivot]->methodPointer - addr;

        if (comparison == 0)
        {
            return g_Methods[pivot];
        }
        else if (comparison > 0)
        {
            right = pivot - 1;
        }
        else
        {
            left = pivot + 1;
        }
    }
    return g_Methods[right];
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

            auto name = hookerData->method->getName();
            char buffer[256]{0};
            sprintf(buffer, "%p | %s::%s", (void *)hookerData->method->getAbsAddress(),
                    hookerData->method->getClass()->getName(), name);
            if (!HookerData::visited.empty())
            {
                for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
                {
                    if (it->name == buffer)
                    {
                        it->goneTime = 10.f;
                        it->time = 2.f;
                        it->hitCount++;
                        // std::rotate(HookerData::visited.rbegin(), it + 1, HookerData::visited.rend());
                        return;
                    }
                }
                // LOGD("%s", hookerData->method->getName());
            }
            HookerData::visited.push_back({buffer, 2.f, 10.f, 0});
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
