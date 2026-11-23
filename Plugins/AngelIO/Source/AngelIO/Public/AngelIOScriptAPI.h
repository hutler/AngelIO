#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

class asIScriptEngine;
class UWorld;

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
    bool IsComplete() const { return bComplete; }
protected:
    void MarkComplete() { check(IsInGameThread()); bComplete = true; }
private:
    bool bComplete = false;
};

namespace AngelIO
{
    // Only available inside Run on an authoritative world-backed mission.
    ANGELIO_API UWorld* GetScriptWorld();
    // A completed operation returns inline. A pending operation suspends Run.
    ANGELIO_API void AwaitOperation(TSharedRef<FAngelIOOperation> Operation, const FString& Label);
}
