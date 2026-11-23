#include "AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"

AAngelIOPlayerController::AAngelIOPlayerController()
{
    Panel = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("LiftPanel"));
    Panel->Script = TEXT("LiftPanel.as");
    TripPanel = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("TripPanel"));
    TripPanel->Script = TEXT("TypedLift.as");
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

void AAngelIOPlayerController::LiftTrip(int32 Floor, float Speed)
{
    ServerRequestTrip(Floor, Speed);
}

void AAngelIOPlayerController::ServerRequestTrip_Implementation(int32 Floor, float Speed)
{
    if (Floor < 1 || Floor > 8 || !FMath::IsFinite(Speed) || Speed <= 0 || Speed > 2000 ||
        TripPanel->GetWaitingSignal() != TEXT("CallLift")) return;
    FAngelIOValue Input;
    Input.Type = EAngelIOType::Float;
    Input.Float = Speed;
    TripPanel->SetInput(TEXT("Speed"), Input);
    TripPanel->SendSignal(TEXT("CallLift"), Floor);
}
