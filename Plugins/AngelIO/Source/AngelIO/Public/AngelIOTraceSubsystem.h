#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOTraceSubsystem.generated.h"

class FJsonObject;

// One bounded mission history per authoritative game world.
UCLASS()
class ANGELIO_API UAngelIOTraceSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Deinitialize() override;
    bool Export(FString& OutFile) const;
    void RecordCheckpoint(bool bSaved, const FGuid& CheckpointId, const FString& Stage, int32 Version,
        const FGuid& SavedBy, TSharedRef<FJsonObject> Caller);
#if WITH_EDITOR
    int64 GetInspectionRevision() const { return Total + ForgottenTargets; }
    int64 GetDroppedEventCount() const { return Total - Events.Num(); }
    void VisitEvents(TFunctionRef<void(const FJsonObject&, const UObject*)> Visitor) const;
    void VisitExecuting(const UObject* Component, const UObject* Actor, TFunctionRef<void(const FJsonObject&)> Visitor) const;
#endif

private:
    friend class FAngelIOOperation;
    TSharedRef<FJsonObject> Append(const FGuid& OperationId, const TCHAR* EventType, const UObject* Target);
    static constexpr int32 Capacity = 4096;
    struct FEntry
    {
        TSharedPtr<FJsonObject> Json;
#if WITH_EDITOR
        TWeakObjectPtr<const UObject> Target;
#endif
    };
    TArray<FEntry> Events;
#if WITH_EDITOR
    // Keep the identity of an executing operation even if its call scrolls out of the journal.
    TMap<FGuid, FEntry> LiveTargets;
    int64 ForgottenTargets = 0;
#endif
    FGuid SessionId;
    FString NetMode;
    int64 Total = 0;
};
