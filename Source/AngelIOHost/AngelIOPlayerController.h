#pragma once

#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "AngelIOPlayerController.generated.h"

class UAngelIOScriptComponent;

UCLASS()
class AAngelIOPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    AAngelIOPlayerController();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> Panel;

    UFUNCTION(Exec)
    void LiftFloor(int32 Floor);
    // An owned controller can request an interaction with a shared world actor.
    UFUNCTION(Server, Reliable, BlueprintCallable, Category="Demo")
    void ServerRequestLiftFloor(int32 Floor);
};

UCLASS()
class AAngelIODemoGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AAngelIODemoGameMode();
};
