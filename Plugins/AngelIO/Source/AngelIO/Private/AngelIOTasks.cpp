#include "AngelIOTasks.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include <string>
THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

namespace AngelIOTasks
{
namespace
{
TSharedRef<FJsonObject> Args(FGuid Mission, FGuid Parent, int32 Argument)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("missionId"), Mission.ToString(EGuidFormats::DigitsWithHyphensLower));
    Obj->SetStringField(TEXT("parentId"), Parent.ToString(EGuidFormats::DigitsWithHyphensLower));
    Obj->SetNumberField(TEXT("argument"), Argument);
    return Obj;
}
template<class T>
struct THandle
{
    int32 Refs = 1;
    TSharedRef<T> Data;
    explicit THandle(TSharedRef<T> In) : Data(MoveTemp(In)) {}
    static void Add(asIScriptGeneric* C) { ++static_cast<THandle*>(C->GetObject())->Refs; }
    static void Release(asIScriptGeneric* C) { auto* H = static_cast<THandle*>(C->GetObject()); if (!--H->Refs) delete H; }
    static bool Bind(asIScriptEngine& E, const char* Name)
    {
        return E.RegisterObjectType(Name, 0, asOBJ_REF) >= 0 &&
            E.RegisterObjectBehaviour(Name, asBEHAVE_ADDREF, "void f()", asFUNCTION(Add), asCALL_GENERIC) >= 0 &&
            E.RegisterObjectBehaviour(Name, asBEHAVE_RELEASE, "void f()", asFUNCTION(Release), asCALL_GENERIC) >= 0;
    }
};
using FTask = THandle<FRecord>;
FRecord& Self(asIScriptGeneric* C) { return *static_cast<FTask*>(C->GetObject())->Data; }
void StringResult(asIScriptGeneric* C, const FString& Value)
{
    new (C->GetAddressOfReturnLocation()) std::string(TCHAR_TO_UTF8(*Value));
}
void State(asIScriptGeneric* C) { C->SetReturnDWord(asDWORD(Self(C).State)); }
void Id(asIScriptGeneric* C) { StringResult(C, Self(C).GetOperationId().ToString(EGuidFormats::DigitsWithHyphensLower)); }
void Parent(asIScriptGeneric* C) { StringResult(C, Self(C).Parent.ToString(EGuidFormats::DigitsWithHyphensLower)); }
void Reason(asIScriptGeneric* C) { StringResult(C, Self(C).Reason); }
void Result(asIScriptGeneric* C)
{
    auto& Task = Self(C);
    *static_cast<int32*>(C->GetArgAddress(0)) = Task.Result;
    C->SetReturnByte(Task.State == EStatus::Succeeded);
}
void Cancel(asIScriptGeneric* C)
{
    auto& Task = Self(C);
    const bool Allowed = !Task.IsComplete() && Task.Parent == ActiveId() && Task.Mission == ActiveMission();
    if (Allowed) Task.Finish(EStatus::Cancelled, TEXT("OwnerCancelled"));
    C->SetReturnByte(Allowed);
}
struct FSelection
{
    bool Ready = false;
    int32 Index = -1;
    EStatus First = EStatus::Running, Second = EStatus::Running;
};
using FJoin = THandle<FSelection>;
FSelection& Selection(asIScriptGeneric* C) { return *static_cast<FJoin*>(C->GetObject())->Data; }
void Ready(asIScriptGeneric* C) { C->SetReturnByte(Selection(C).Ready); }
void Index(asIScriptGeneric* C) { C->SetReturnDWord(Selection(C).Index); }
void First(asIScriptGeneric* C) { C->SetReturnDWord(asDWORD(Selection(C).First)); }
void Second(asIScriptGeneric* C) { C->SetReturnDWord(asDWORD(Selection(C).Second)); }
void Watch(asIScriptGeneric* C, bool Pair, bool All)
{
    auto* A = static_cast<FTask*>(Pair ? C->GetArgObject(0) : C->GetObject());
    auto* B = Pair ? static_cast<FTask*>(C->GetArgObject(1)) : A;
    const float Seconds = C->GetArgFloat(Pair ? 2 : 0);
    if (!A || !B || A->Data->Mission != ActiveMission() || B->Data->Mission != ActiveMission() ||
        !FMath::IsFinite(Seconds) || (Seconds < 0 && Seconds != -1) ||
        A->Data->GetOperationId() == ActiveId() || B->Data->GetOperationId() == ActiveId())
    {
        asGetActiveContext()->SetException("Task watches require other tasks of this mission and a valid timeout");
        return;
    }
    auto Snapshot = MakeShared<FSelection>();
    TOptional<double> Deadline;
    if (Seconds >= 0) Deadline = Seconds;
    AngelIO::AwaitCondition(TEXT("Task.Join"), A->Data->GetOperationId().ToString(EGuidFormats::DigitsWithHyphensLower), Deadline,
        [Left=A->Data, Right=B->Data, All, Snapshot]
        {
            Snapshot->First = Left->State;
            Snapshot->Second = Right->State;
            Snapshot->Ready = All ? Left->IsComplete() && Right->IsComplete() : Left->IsComplete() || Right->IsComplete();
            if (Snapshot->Ready) Snapshot->Index = Left->IsComplete() ? 0 : 1;
            return Snapshot->Ready;
        });
    C->SetReturnAddress(new FJoin(Snapshot));
}
void Join(asIScriptGeneric* C) { Watch(C, false, true); }
void WatchFirst(asIScriptGeneric* C) { Watch(C, true, false); }
void WatchAll(asIScriptGeneric* C) { Watch(C, true, true); }
void NextFrame(asIScriptGeneric*)
{
    TWeakObjectPtr<UWorld> World = AngelIO::GetScriptWorld();
    if (!World.IsValid()) { asGetActiveContext()->SetException("NextFrame needs a game world"); return; }
    const double Now = World->GetTimeSeconds();
    AngelIO::AwaitCondition(TEXT("NextFrame"), {}, {}, [World, Now]{ return World.IsValid() && World->GetTimeSeconds() > Now; });
}
}

FRecord::FRecord(UWorld* World, const FString& Function, FGuid MissionId, FGuid ParentId, int32 Argument, const UObject* Component)
    : FAngelIOOperation(World, TEXT("Task.Start"), Function, Args(MissionId, ParentId, Argument), Component),
      Mission(MissionId), Parent(ParentId) {}
void FRecord::Finish(EStatus End, FString Why, int32 Value)
{
    if (IsComplete()) return;
    State = End;
    Reason = MoveTemp(Why);
    Result = Value;
    MarkComplete(End == EStatus::Succeeded ? TEXT("Succeeded") : End == EStatus::Cancelled ? TEXT("Cancelled") : TEXT("Failed"), Reason);
}
void ReturnHandle(asIScriptGeneric* Call, const TSharedRef<FRecord>& Record) { Call->SetReturnAddress(new FTask(Record)); }

bool Register(asIScriptEngine& E)
{
    if (E.RegisterEnum("TaskStatus") < 0) return false;
    int32 N = 0;
    for (const char* S : {"Running","Waiting","Succeeded","Cancelled","Failed"})
        if (E.RegisterEnumValue("TaskStatus", S, N++) < 0) return false;
    return FTask::Bind(E, "Task") && FJoin::Bind(E, "TaskJoin") &&
        E.RegisterFuncdef("int MissionWork(int argument)") >= 0 &&
        E.RegisterObjectMethod("Task","TaskStatus get_Status() const property",asFUNCTION(State),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","string get_Id() const property",asFUNCTION(Id),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","string get_ParentId() const property",asFUNCTION(Parent),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","string get_Reason() const property",asFUNCTION(Reason),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","bool TryGetResult(int &out) const",asFUNCTION(Result),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","bool Cancel()",asFUNCTION(Cancel),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("Task","TaskJoin@ Join(float seconds = -1)",asFUNCTION(Join),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("TaskJoin","bool get_Ready() const property",asFUNCTION(Ready),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("TaskJoin","int get_Index() const property",asFUNCTION(Index),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("TaskJoin","TaskStatus get_First() const property",asFUNCTION(First),asCALL_GENERIC) >= 0 &&
        E.RegisterObjectMethod("TaskJoin","TaskStatus get_Second() const property",asFUNCTION(Second),asCALL_GENERIC) >= 0 &&
        E.RegisterGlobalFunction("TaskJoin@ WatchFirst(Task@+ a, Task@+ b, float seconds = -1)",asFUNCTION(WatchFirst),asCALL_GENERIC) >= 0 &&
        E.RegisterGlobalFunction("TaskJoin@ WatchAll(Task@+ a, Task@+ b, float seconds = -1)",asFUNCTION(WatchAll),asCALL_GENERIC) >= 0 &&
        E.RegisterGlobalFunction("void NextFrame()",asFUNCTION(NextFrame),asCALL_GENERIC) >= 0;
}
}
