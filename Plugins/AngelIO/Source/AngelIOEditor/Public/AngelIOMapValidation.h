#pragma once

#include "CoreMinimal.h"

class UWorld;
class UAngelIOScriptComponent;

struct FAngelIOMapDiagnostic
{
    FString Code, Severity, Message;
    const UAngelIOScriptComponent* Component = nullptr;
    FString Field;
    int32 Line = 0;
    int32 Column = 0;
};

struct ANGELIOEDITOR_API FAngelIOMapReport
{
    FString Map;
    int32 MissionsChecked = 0;
    TArray<FAngelIOMapDiagnostic> Diagnostics;
    TArray<const AActor*> Actors;
    TArray<const UAngelIOScriptComponent*> Components;
    struct FRole
    {
        FString Component, Role, Endpoint, Function, ExpectedClass;
        int32 Line;
        bool bRoom, bSingle, bMayStream;
        TArray<FString> Matches, WrongClass;
    };
    TArray<FRole> Roles;
    bool IsValid() const;
};

namespace AngelIO
{
    // Reads currently loaded level actors and the map's default player-controller missions.
    ANGELIOEDITOR_API FAngelIOMapReport ValidateMap(UWorld& World);
}
