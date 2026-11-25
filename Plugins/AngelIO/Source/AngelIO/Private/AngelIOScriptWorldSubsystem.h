#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOBytecodeCacheStats.h"
#include "AngelIOScriptWorldSubsystem.generated.h"

class FAngelIOProgram;
struct FAngelIORegisteredEngine;

// Runtime missions register the API lazily on the authority game thread.
UCLASS()
class UAngelIOScriptWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void Deinitialize() override;
    bool DoesSupportWorldType(EWorldType::Type Type) const override
    {
        return Type == EWorldType::Game || Type == EWorldType::PIE || Type == EWorldType::GamePreview;
    }

private:
    friend class FAngelIOProgram;
    friend struct FAngelIOStartupProbe;
    friend TOptional<FAngelIOBytecodeCacheStats> AngelIO::GetBytecodeCacheStats(UWorld*);
    TSharedPtr<FAngelIORegisteredEngine> RegisteredEngine;
    TSet<FAngelIOProgram*> Programs;
    bool bRegistering = false;
    bool bClosing = false;
};
