#include "AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"

AAngelIOPlayerController::AAngelIOPlayerController()
{
    Panel = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("LiftPanel"));
    Panel->Script = TEXT("LiftPanel.as");
}

AAngelIODemoGameMode::AAngelIODemoGameMode()
{
    PlayerControllerClass = AAngelIOPlayerController::StaticClass();
}

void AAngelIOPlayerController::ServerRequestLiftFloor_Implementation(int32 Floor)
{
    if (Floor >= 1 && Floor <= 8)
        Panel->SendSignal(TEXT("SelectFloor"), Floor);
}

void AAngelIOPlayerController::LiftFloor(int32 Floor)
{
    ServerRequestLiftFloor(Floor);
}
