#pragma once
#include "Il2cpp/il2cpp-class.h"
#include "Tool/Tool.h"

namespace Frida
{
    struct TraceData
    {
        MethodInfo *method = nullptr;
        std::string msg;
    };
    void Init();
    bool Trace(MethodInfo *method, HookerData *data);
    bool Untrace(MethodInfo *method);
    bool isTraced(MethodInfo *method);
} // namespace Frida
