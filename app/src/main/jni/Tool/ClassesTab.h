#pragma once

#include "Il2cpp/il2cpp-class.h"
#include "Includes/circular_buffer.h"
#include "Tool/PopUpSelector.h"
#include <set>
struct ClassesTab
{
    using Object = Il2CppObject *;
    using Class = Il2CppClass *;
    using Json = nlohmann::ordered_json;
    using Paths = std::vector<std::string>;
    using DataPair = std::pair<std::pair<Il2CppObject *, Json>, Paths>;

    using MethodParamList = std::vector<std::pair<const char *, Il2CppType *>>;
    using MethodList = std::vector<std::pair<MethodInfo *, MethodParamList>>;
    using ClassMethodMap = std::unordered_map<Class, MethodList>;

    struct CallData
    {
        MethodInfo *method;
        Il2CppObject *thiz;
        Il2CppArray<Il2CppObject *> *params;
    };

    struct ParamValue
    {
        std::string value;
        Il2CppObject *object;
    };

    std::unordered_map<Object, DataPair> dataMap{};

    bool caseSensitive = true;
    bool filterByClass = true;
    bool filterByMethod = false;
    bool filterByField = false;
    bool showAllClasses = false;
    bool includeAllImages = false;

    std::map<Il2CppObject *, bool> tabMap{};

    Il2CppImage *selectedImage = nullptr;

    std::vector<Class> classes{};
    std::vector<Class> filteredClasses{};
    std::vector<Il2CppClass *> tracer{};
    std::vector<MethodInfo *> tracedMethods;

    static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> objectMap;
    static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> newObjectMap;
    static std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> savedSet;
    std::unordered_map<MethodInfo *, std::unordered_map<std::string, ParamValue>> paramMap{};
    std::unordered_map<MethodInfo *, CircularBuffer<std::pair<std::string, Il2CppObject *>>> callResults{};

    MethodList &buildMethodMap(Il2CppClass *klass);

    ClassMethodMap methodMap{};
    ClassesTab();

    Paths &getJsonPaths(Il2CppObject *object);

    void setJsonObject(Il2CppObject *object);

    Json &getJsonObject(Il2CppObject *object);

    void ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                             std::function<void(Il2CppObject *)> onSelect, bool canNew = false);

    void CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    bool isMethodHooked(MethodInfo *method);

    void PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                     Il2CppObject *thiz = nullptr);

    void HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    const MethodParamList &getCachedParams(MethodInfo *method);

    struct OriginalMethodBytes
    {
        std::vector<uint8_t> bytes;
        std::string text;
    };
    static std::unordered_map<MethodInfo *, OriginalMethodBytes> oMap;
    static PopUpSelector poper; // still a prototype!
    bool MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                      Il2CppObject *thiz = nullptr, bool includeInflated = false);

    static std::unordered_map<Il2CppClass *, bool> states;
    void ClassViewer(Il2CppClass *klass);

    std::string filter = "";
    int selectedImageIndex = -1;
    bool traceState = false;

    int maxProgress = 0;
    int progress = 0;
    void Draw(int index = -1, bool closeable = false);

    bool opened = true;
    bool currentlyOpened = false;
    bool setOpenedTab = false;
    void DrawTabMap();

    void ImGuiJson(Il2CppObject *object);

    void FilterClasses(const std::string &filter);
};

void to_json(nlohmann::ordered_json &j, const ClassesTab &p);
void from_json(const nlohmann::ordered_json &j, ClassesTab &p);
void hookerHandler(void *address, DobbyRegisterContext *ctx);