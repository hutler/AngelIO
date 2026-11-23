#pragma once

#include "Commandlets/Commandlet.h"
#include "AngelIOCheckCommandlet.generated.h"

UCLASS()
class UAngelIOCheckCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UAngelIOCheckCommandlet();
    virtual int32 Main(const FString& Params) override;
};
