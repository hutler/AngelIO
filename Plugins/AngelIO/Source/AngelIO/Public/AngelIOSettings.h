#pragma once

#include "Engine/DeveloperSettings.h"
#include "AngelIOSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="AngelIO"))
class ANGELIO_API UAngelIOSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    // Retained bytecode cache memory per authority world. Zero disables caching.
    UPROPERTY(Config, EditAnywhere, Category="Missions", meta=(ClampMin="0", DisplayName="Bytecode Cache Budget (KiB)"))
    int32 BytecodeCacheBudgetKiB = 256;
};
