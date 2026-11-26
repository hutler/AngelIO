#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class asIScriptEngine;
class UWorld;
class UAngelIOScriptComponent;
class FAngelIOOperation;
class FJsonObject;
class UAngelIOTraceSubsystem;

#if WITH_EDITOR
// Global script calls whose first argument addresses actors placed on the map.
struct FAngelIOMapLookup
{
    FString Function;
    UClass* OwnerClass;
    bool bRequireSingleActor;
    // Global waits may intentionally name a streamed actor. Explicit room bindings are checked locally.
    bool bAllowUnloadedGlobal = false;
};
#endif

enum class EAngelIOWaitOutcome : uint8 { Pending, Completed, TimedOut, Detached };

struct FAngelIOScriptWrapper
{
    FString Declaration;
    FString Body;
};

namespace AngelIO
{
    ANGELIO_API UWorld* GetScriptWorld();
    ANGELIO_API UAngelIOScriptComponent* GetScriptMission();
    // Identity of the last saved or matching loaded checkpoint (writer, version and stage).
    // Invalid outside a mission or after a checkpoint lookup misses.
    ANGELIO_API FGuid GetCheckpointStepId();
    // A completed operation returns inline. A pending operation suspends Run.
    ANGELIO_API void AwaitOperation(TSharedRef<FAngelIOOperation> Operation, const FString& Label);
    // One outcome per reader. An empty timeout waits indefinitely; zero polls.
    ANGELIO_API TSharedRef<const EAngelIOWaitOutcome> AwaitOperation(TSharedRef<FAngelIOOperation> Operation,
        const FString& Label, TOptional<double> TimeoutSeconds);
    // Supply a side-effect-free game-thread predicate and capture world objects weakly.
    // Checks immediately and on each update of this waiting VM until ready or expired.
    ANGELIO_API void AwaitCondition(const FString& Label, const FString& Target, TOptional<double> TimeoutSeconds,
        TFunction<bool()> IsReady);
}

// Register before the first mission compiles. Each authority world keeps its registered
// native bindings until world teardown, including periods with zero live missions.
// Worldless validation creates an independent engine. Keep providers loaded for both lifetimes.
class ANGELIO_API IAngelIOScriptAPI : public IModularFeature
{
public:
    static FName FeatureName() { return TEXT("AngelIOScriptAPI"); }
    virtual bool RegisterBindings(asIScriptEngine& Engine) = 0;
    // A wrapper can return a typed value after a suspending native call resumes.
    virtual void GetScriptWrappers(TArray<FAngelIOScriptWrapper>& Out) const {}
#if WITH_EDITOR
    virtual void GetMapLookups(TArray<FAngelIOMapLookup>& Out) const {}
#endif
};

// Game-thread completion shared by native gameplay, script handles and waiters.
class ANGELIO_API FAngelIOOperation
{
public:
    explicit FAngelIOOperation(UWorld* World = nullptr, const FString& Name = TEXT("Operation"),
        const FString& Target = {}, TSharedPtr<FJsonObject> Arguments = nullptr, const UObject* TraceTarget = nullptr);
    ~FAngelIOOperation();
    FAngelIOOperation(const FAngelIOOperation&) = delete;
    FAngelIOOperation& operator=(const FAngelIOOperation&) = delete;
    const FGuid& GetOperationId() const { return OperationId; }
    bool IsComplete() const { return bComplete; }
protected:
    void MarkComplete(const FString& Status = TEXT("Completed"), const FString& Reason = {});
private:
    friend class FAngelIOProgram;
    friend TSharedRef<const EAngelIOWaitOutcome> AngelIO::AwaitOperation(TSharedRef<FAngelIOOperation>, const FString&, TOptional<double>);
    TSharedPtr<FJsonObject> AddEvent(const TCHAR* Type) const;
    FGuid OperationId;
    TWeakObjectPtr<UAngelIOTraceSubsystem> Trace;
#if WITH_EDITOR
    TWeakObjectPtr<const UObject> InspectionTarget;
#endif
    bool bComplete = false;
};
