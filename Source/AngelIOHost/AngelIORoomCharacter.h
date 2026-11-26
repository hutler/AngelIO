#pragma once

#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "AngelIORoomCharacter.generated.h"

UCLASS()
class AAngelIORoomCharacter : public ACharacter
{
    GENERATED_BODY()
    void SetupPlayerInputComponent(UInputComponent* Input) override;
    void MoveForward(float Value);
    void MoveRight(float Value);
};

UCLASS()
class AAngelIORoomGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AAngelIORoomGameMode();
};
