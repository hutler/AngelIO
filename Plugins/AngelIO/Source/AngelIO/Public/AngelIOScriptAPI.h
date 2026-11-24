#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class asIScriptEngine;
class UWorld;
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

// Register in StartupModule before any mission is compiled. Bindings must remain
// loaded for the lifetime of every VM that uses them.
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
