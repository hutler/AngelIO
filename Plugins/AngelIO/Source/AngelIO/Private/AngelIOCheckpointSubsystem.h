#pragma once

#include "Containers/Utf8String.h"
#include "SQLiteDatabase.h"
#include "Subsystems/WorldSubsystem.h"
#include "AngelIOCheckpointSubsystem.generated.h"

class UActorComponent;

// The authoritative world opens its checkpoint slot on first use.
UCLASS()
class UAngelIOCheckpointSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    struct FCheckpoint
    {
        FGuid Id;
        FGuid SavedBy;
        int32 Version = 0;
        FUtf8String Stage;
    };
    virtual void Deinitialize() override;
    // Version zero denotes a missing row. A storage error must never start a mission fresh.
    bool Load(const UActorComponent& Component, const FString& Script, FCheckpoint& Out, FString& Error, bool bInspectOnly = false);
    bool Save(const UActorComponent& Component, const FString& Script, FCheckpoint& Record, FString& Error);

private:
    bool Open(FString& Error, bool bInspectOnly);
    FSQLiteDatabase Database;
};
