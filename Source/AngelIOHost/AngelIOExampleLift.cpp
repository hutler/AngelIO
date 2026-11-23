#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIOScriptComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

AAngelIOExampleLift::AAngelIOExampleLift()
{
    bReplicates = true;
    SetReplicateMovement(true);
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    auto* Platform = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Platform"));
    SetRootComponent(Platform);
    Platform->SetMobility(EComponentMobility::Movable);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Platform->SetStaticMesh(Cube.Object);
    Platform->SetRelativeScale3D(FVector(2, 2, 0.2));
    Mission = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("Mission"));
    Mission->Script = TEXT("Elevator.as");
    Mission->SetTargetName(TEXT("DemoLifts"));
    Mission->PresentationSignals.Add(TEXT("Arrived"));
    Address = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("Address"));
    Address->bStartOnBeginPlay = false;
}

void AAngelIOExampleLift::BeginPlay()
{
    Mission->OnSignal.AddDynamic(this, &AAngelIOExampleLift::HandleSignal);
    Mission->OnPublicStateChanged.AddDynamic(this, &AAngelIOExampleLift::CheckMotionState);
    Super::BeginPlay();
    if (!HasAuthority() || GetNetMode() == NM_Client) return;
    FAngelIOValue Floor;
    if (Mission->GetOutput(TEXT("LastFloor"), Floor))
        OriginZ = GetActorLocation().Z - double(Floor.Integer) * 200.0;
    if (bRequestOnStart) RequestFloor(RequestedFloor);
}

bool AAngelIOExampleLift::RequestFloor(int32 Floor)
{
    return Mission->SendSignal(TEXT("RequestFloor"), Floor);
}

TSharedRef<FLiftTrip> AAngelIOExampleLift::MoveTo(int32 Floor, float TravelSpeed)
{
    auto Trip = MakeShared<FLiftTrip>(Floor);
    if (!HasAuthority() || GetNetMode() == NM_Client || ActiveTrip || Floor < 1 || Floor > 8 ||
        !FMath::IsFinite(TravelSpeed) || TravelSpeed <= 0 || Mission->GetWaitingSignal() != TEXT("RequestFloor"))
    {
        Trip->Finish(ELiftTripStatus::Rejected);
        return Trip;
    }
    FAngelIOValue Override;
    Override.Type = EAngelIOType::Float;
    Override.Float = TravelSpeed;
    if (!Mission->SetInput(TEXT("CommandSpeed"), Override))
    {
        Trip->Finish(ELiftTripStatus::Rejected);
        return Trip;
    }
    ActiveTrip = Trip;
    RequestFloor(Floor);
    Override.Float = 0;
    Mission->SetInput(TEXT("CommandSpeed"), Override);
    if (ActiveTrip == Trip && Mission->GetWaitingSignal() != TEXT("MotionFinished"))
    {
        ActiveTrip.Reset();
        Trip->Finish(ELiftTripStatus::Rejected);
    }
    return Trip;
}

void AAngelIOExampleLift::EndPlay(const EEndPlayReason::Type Reason)
{
    if (ActiveTrip) ActiveTrip->Finish(ELiftTripStatus::Cancelled);
    ActiveTrip.Reset();
    Super::EndPlay(Reason);
}

void AAngelIOExampleLift::CheckMotionState()
{
    if (HasAuthority() && IsActorTickEnabled() && Mission->GetWaitingSignal() != TEXT("MotionFinished"))
    {
        SetActorTickEnabled(false);
        if (ActiveTrip) ActiveTrip->Finish(ELiftTripStatus::Cancelled);
        ActiveTrip.Reset();
    }
}

void AAngelIOExampleLift::HandleSignal(FString Name, int32 Value)
{
    if (Name == TEXT("Arrived"))
    {
        if (ActiveTrip)
        {
            const bool bArrived = Value == ActiveTrip->Floor && AtDestination();
            ActiveTrip->Finish(bArrived ? ELiftTripStatus::Arrived : ELiftTripStatus::Cancelled);
            ActiveTrip.Reset();
        }
        UE_LOG(LogTemp, Display, TEXT("AngelIO demo: %s arrived at floor %d"), *GetName(), Value);
        return;
    }
    if (Name != TEXT("MoveTo")) return;
    FAngelIOValue TripSpeed;
    if (!Mission->GetOutput(TEXT("TripSpeed"), TripSpeed)) return;
    Speed = ActiveTrip ? TripSpeed.Float : FMath::Max(1.0f, TripSpeed.Float);
    DestinationFloor = Value;
    DestinationZ = OriginZ + double(Value) * 200.0;
    StartZ = GetActorLocation().Z;
    ElapsedTravelTime = 0;
    if (AtDestination())
        Mission->SendSignal(TEXT("MotionFinished"), DestinationFloor);
    else
        SetActorTickEnabled(true);
}

bool AAngelIOExampleLift::AtDestination() const
{
    // Unreal may coalesce a sub-millimetre transform update at the end of a trip.
    return FMath::IsNearlyEqual(GetActorLocation().Z, DestinationZ, 0.01);
}

void AAngelIOExampleLift::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    FVector Location = GetActorLocation();
    ElapsedTravelTime += DeltaSeconds;
    Location.Z = FMath::FInterpConstantTo(StartZ, DestinationZ, ElapsedTravelTime, double(Speed));
    SetActorLocation(Location);
    if (AtDestination())
    {
        SetActorTickEnabled(false);
        Mission->SendSignal(TEXT("MotionFinished"), DestinationFloor);
    }
}
