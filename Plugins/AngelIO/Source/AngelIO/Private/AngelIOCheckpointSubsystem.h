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
    void CloseSlot() { Database.Close(); }
    // Version zero denotes a missing row. A storage error must never start a mission fresh.
    bool Load(const UActorComponent& Component, const FString& Script, FCheckpoint& Out, FString& Error, bool bInspectOnly = false);
    bool Save(const UActorComponent& Component, const FString& Script, FCheckpoint& Record, FString& Error);
    bool LoadState(const UActorComponent& Component, const FString& Key, TArray<uint8>& Data, FString& Error);
    bool SaveState(const UActorComponent& Component, const FString& Key, TConstArrayView<uint8> Data, FString& Error);
    bool RecordOnce(const UActorComponent& Component, const FString& Script, const FString& Action, FGuid& Receipt, FString& Error);
    FGuid StateId(const UActorComponent& Component, const FString& Key) const;

private:
    bool Open(FString& Error, bool bInspectOnly);
    struct FKey
    {
        FString Map, Owner, Name;
        bool Bind(FSQLitePreparedStatement& Statement, int32 First = 1) const;
        bool operator==(const FKey& Other) const { return Map == Other.Map && Owner == Other.Owner && Name == Other.Name; }
    };
    bool ResolveKey(const UActorComponent& Component, FString Name, bool bMission, bool bInspectOnly, FKey& Out, FString& Error);
    bool bReadOnly = false;
    FSQLiteDatabase Database;
    FString TraceSlot;
};
