#pragma once

#include "AngelIOScriptAPI.h"
class asIScriptGeneric;

namespace AngelIOTasks
{
    enum class EStatus : uint8 { Running, Waiting, Succeeded, Cancelled, Failed };
    struct FRecord : FAngelIOOperation
    {
        FRecord(UWorld* World, const FString& Function, FGuid MissionId, FGuid ParentId, int32 Argument, const UObject* Component);
        void Finish(EStatus End, FString Why = {}, int32 Value = 0);
        FGuid Mission, Parent;
        EStatus State = EStatus::Running;
        int32 Result = 0;
        FString Reason;
    };
    FGuid ActiveId();
    FGuid ActiveMission();
    bool Register(asIScriptEngine& Engine);
    void ReturnHandle(asIScriptGeneric* Call, const TSharedRef<FRecord>& Record);
}
