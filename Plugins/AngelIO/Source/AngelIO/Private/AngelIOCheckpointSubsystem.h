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
    FUtf8String Load(const UActorComponent& Component, const FString& Script, int32 Version) const;
    void Save(const UActorComponent& Component, const FString& Script, int32 Version, FUtf8String Stage);

private:
    struct FCheckpoint
    {
        int32 Version;
        FUtf8String Stage;
    };
    using FKey = TPair<FString, FString>;
    static FKey MakeKey(const UActorComponent& Component, FString Script);
    TMap<FKey, FCheckpoint> Records;
};
