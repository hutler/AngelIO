#pragma once

#include "CoreMinimal.h"

class UAngelIOScriptComponent;

// Room is the placement of a level instance; Placement is the component in the template.
struct ANGELIO_API FAngelIOPersistenceIdentity
{
    FString Map, Room, Owner, LegacyMap, LegacyRoom, LegacyOwner;
    TArray<FString> LegacyOwners;
    FGuid Placement;
    static FAngelIOPersistenceIdentity From(const UAngelIOScriptComponent& Mission);
    FString MissionKey(const UAngelIOScriptComponent& Mission, FString Script) const;
    bool ConflictsWith(const FAngelIOPersistenceIdentity& Other) const;
    // Checks live peers before adopting a legacy row or writing a stable key.
    bool CheckPeers(const UAngelIOScriptComponent& Mission, FString& Error) const;
};
