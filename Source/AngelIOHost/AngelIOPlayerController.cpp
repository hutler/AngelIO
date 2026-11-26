#include "AngelIOPlayerController.h"
#include "AngelIOScriptComponent.h"
#include "AngelIORoomTerminal.h"
#include "Components/InputComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "InputCoreTypes.h"

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

void AAngelIORoomPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AAngelIORoomPlayerController::UseTerminal);
}

void AAngelIORoomPlayerController::UseTerminal()
{
    const APawn* UserPawn = GetPawn();
    if (!UserPawn) return;
    AAngelIORoomTerminal* Closest = nullptr;
    double BestDistance = TNumericLimits<double>::Max();
    for (TActorIterator<AAngelIORoomTerminal> It(GetWorld()); It; ++It)
    {
        if (!It->IsInReach(UserPawn)) continue;
        const double Distance = FVector::DistSquared(It->GetActorLocation(), UserPawn->GetActorLocation());
        if (Distance < BestDistance)
        {
            Closest = *It;
            BestDistance = Distance;
        }
    }
    if (Closest) ServerUseTerminal(Closest, FName(*Closest->Mission->GetWaitingSignal()));
}

void AAngelIORoomPlayerController::ServerUseTerminal_Implementation(AAngelIORoomTerminal* Terminal, FName ExpectedInput)
{
    if (!IsValid(Terminal) || !Terminal->IsInReach(GetPawn())) return;
    const FString Waiting = Terminal->Mission->GetWaitingSignal();
    if (ExpectedInput.IsNone()) ExpectedInput = TEXT("Activate"); // Original single-action terminal.
    if (ExpectedInput == FName(*Waiting) && (Waiting == TEXT("Activate") || Waiting == TEXT("Dispatch") ||
        Waiting == TEXT("Retry") || Waiting == TEXT("Confirm")))
        Terminal->Mission->SendSignalFrom(Waiting, 0, *this);
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
