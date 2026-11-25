#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOScriptWorldSubsystem.generated.h"

class FAngelIOProgram;
class FAngelIOBytecodeCache;
struct FAngelIORegisteredEngine;

// Runtime missions register the API lazily on the authority game thread.
UCLASS()
class UAngelIOScriptWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    static const FAngelIOBytecodeCache* FindBytecodeCache(UWorld* World);
    void Deinitialize() override;
    bool DoesSupportWorldType(EWorldType::Type Type) const override
    {
        return Type == EWorldType::Game || Type == EWorldType::PIE || Type == EWorldType::GamePreview;
    }

private:
    friend class FAngelIOProgram;
    friend struct FAngelIOStartupProbe;
    TSharedPtr<FAngelIORegisteredEngine> RegisteredEngine;
    TSet<FAngelIOProgram*> Programs;
    bool bRegistering = false;
    bool bClosing = false;
};
