#pragma once

#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "AngelIOPlayerController.generated.h"

class UAngelIOScriptComponent;
class AAngelIORoomTerminal;

UCLASS()
class AAngelIORoomPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    void SetupInputComponent() override;
    UFUNCTION(Exec)
    void UseTerminal();
    UFUNCTION(Server, Reliable, BlueprintCallable, Category="Demo")
    void ServerUseTerminal(AAngelIORoomTerminal* Terminal, FName ExpectedInput = NAME_None);
};

UCLASS()
class AAngelIOPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    AAngelIOPlayerController();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> Panel;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> TripPanel;

    UFUNCTION(Exec)
    void LiftTrip(int32 Floor, float Speed);
    UFUNCTION(Server, Reliable)
    void ServerRequestTrip(int32 Floor, float Speed);

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
