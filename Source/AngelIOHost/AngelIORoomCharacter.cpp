#include "AngelIORoomCharacter.h"
#include "AngelIOPlayerController.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"

AAngelIORoomGameMode::AAngelIORoomGameMode()
{
    DefaultPawnClass = AAngelIORoomCharacter::StaticClass();
    PlayerControllerClass = AAngelIORoomPlayerController::StaticClass();
}

void AAngelIORoomCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomForward"), EKeys::W, 1));
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomForward"), EKeys::S, -1));
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomRight"), EKeys::D, 1));
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomRight"), EKeys::A, -1));
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomTurn"), EKeys::MouseX, 1));
    UPlayerInput::AddEngineDefinedAxisMapping(FInputAxisKeyMapping(TEXT("DoorRoomLook"), EKeys::MouseY, -1));
    Input->BindAxis(TEXT("DoorRoomForward"), this, &AAngelIORoomCharacter::MoveForward);
    Input->BindAxis(TEXT("DoorRoomRight"), this, &AAngelIORoomCharacter::MoveRight);
    Input->BindAxis(TEXT("DoorRoomTurn"), static_cast<APawn*>(this), &APawn::AddControllerYawInput);
    Input->BindAxis(TEXT("DoorRoomLook"), static_cast<APawn*>(this), &APawn::AddControllerPitchInput);
}

void AAngelIORoomCharacter::MoveForward(float Value)
{
    AddMovementInput(GetActorForwardVector(), Value);
}

void AAngelIORoomCharacter::MoveRight(float Value)
{
    AddMovementInput(GetActorRightVector(), Value);
}
