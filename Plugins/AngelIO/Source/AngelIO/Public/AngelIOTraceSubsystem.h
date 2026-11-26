#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOBytecodeCacheStats.h"
#include "AngelIOTraceSubsystem.generated.h"

class FJsonObject;
class UAngelIOScriptComponent;

// One bounded mission history per authoritative game world.
UCLASS()
class ANGELIO_API UAngelIOTraceSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual void PreDeinitialize() override;
    virtual void Deinitialize() override;
    bool Export(FString& OutFile) const;
    // Related rows share the returned signal ID. Native senders can supply their actor as the caller.
    FGuid SignalSent(const FString& Signal, int32 Value, const FString& Target, int32 Count, TSharedPtr<FJsonObject> Caller);
    void SignalRecipient(FGuid SignalId, const FString& Component, FGuid Instance, const FString& Outcome);
    void RecordDecision(const FString& Operation, const FString& Target, const FString& Reason,
        TSharedRef<FJsonObject> Caller, const UObject* Subject, TSharedPtr<FJsonObject> Arguments = nullptr);
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
    friend class UAngelIOScriptComponent;
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
    const FGuid SessionId = FGuid::NewGuid();
    FString NetMode;
    TOptional<FAngelIOBytecodeCacheSnapshot> FinalCache;
    int64 Total = 0;
};
