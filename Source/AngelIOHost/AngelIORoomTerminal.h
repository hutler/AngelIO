#pragma once

#include "GameFramework/Actor.h"
#include "AngelIORoomTerminal.generated.h"

class UAngelIOScriptComponent;

UCLASS()
class AAngelIORoomTerminal : public AActor
{
    GENERATED_BODY()
public:
    AAngelIORoomTerminal();
    bool IsInReach(const APawn* Pawn) const;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terminal")
    TObjectPtr<UAngelIOScriptComponent> Mission;

    UPROPERTY(VisibleAnywhere, Category="Terminal")
    TObjectPtr<class UPointLightComponent> Lamp;

private:
    UFUNCTION()
    void UpdateLamp();
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type Reason) override;
};
