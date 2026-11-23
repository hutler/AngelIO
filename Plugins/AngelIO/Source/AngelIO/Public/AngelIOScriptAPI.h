#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class asIScriptEngine;
class UWorld;
class FAngelIOOperation;
class FJsonObject;
class UAngelIOTraceSubsystem;

namespace AngelIO
{
    ANGELIO_API UWorld* GetScriptWorld();
    // A completed operation returns inline. A pending operation suspends Run.
    ANGELIO_API void AwaitOperation(TSharedRef<FAngelIOOperation> Operation, const FString& Label);
}

// Register in StartupModule before any mission is compiled. Bindings must remain
// loaded for the lifetime of every VM that uses them.
class ANGELIO_API IAngelIOScriptAPI : public IModularFeature
{
public:
    static FName FeatureName() { return TEXT("AngelIOScriptAPI"); }
    virtual bool RegisterBindings(asIScriptEngine& Engine) = 0;
};

// Game-thread completion shared by native gameplay, script handles and waiters.
class ANGELIO_API FAngelIOOperation
{
public:
    explicit FAngelIOOperation(UWorld* World = nullptr, const FString& Name = TEXT("Operation"),
        const FString& Target = {}, TSharedPtr<FJsonObject> Arguments = nullptr);
    FAngelIOOperation(const FAngelIOOperation&) = delete;
    FAngelIOOperation& operator=(const FAngelIOOperation&) = delete;
    const FGuid& GetOperationId() const { return OperationId; }
    bool IsComplete() const { return bComplete; }
protected:
    void MarkComplete(const FString& Status = TEXT("Completed"), const FString& Reason = {});
private:
    friend class FAngelIOProgram;
    friend void AngelIO::AwaitOperation(TSharedRef<FAngelIOOperation>, const FString&);
    TSharedPtr<FJsonObject> AddEvent(const TCHAR* Type) const;
    FGuid OperationId;
    TWeakObjectPtr<UAngelIOTraceSubsystem> Trace;
    bool bComplete = false;
};
