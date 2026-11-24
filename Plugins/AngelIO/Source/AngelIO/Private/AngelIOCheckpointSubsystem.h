#pragma once

#include "Containers/Utf8String.h"
#include "Subsystems/WorldSubsystem.h"
#include "AngelIOCheckpointSubsystem.generated.h"

class UActorComponent;

// Session progress outlives a streamed level. Records contain no object or VM references.
UCLASS()
class UAngelIOCheckpointSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    struct FCheckpoint
    {
        FGuid Id = FGuid::NewGuid();
        FGuid SavedBy;
        int32 Version = 0;
        FUtf8String Stage;
    };
    const FCheckpoint* Find(const UActorComponent& Component, const FString& Script) const;
    const FCheckpoint& Save(const UActorComponent& Component, const FString& Script, int32 Version,
        FUtf8String Stage, const FGuid& Instance);

private:
    using FKey = TPair<FString, FString>;
    static FKey MakeKey(const UActorComponent& Component, FString Script);
    TMap<FKey, FCheckpoint> Records;
};
