#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIOScriptComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/LevelStreaming.h"
#include "LevelUtils.h"
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
    // The mission restarts at its configured floor on every level activation.
    if (HasAuthority() && GetNetMode() != NM_Client)
    {
        if (const auto* Stream = FLevelUtils::FindStreamingLevel(GetLevel()))
        {
            if (StartPositionInLevel.IsSet())
                SetActorLocation(Stream->LevelTransform.TransformPosition(*StartPositionInLevel), false, nullptr, ETeleportType::TeleportPhysics);
            else
                StartPositionInLevel = Stream->LevelTransform.InverseTransformPosition(GetActorLocation());
        }
    }
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

bool AAngelIOExampleLift::IsReadyForTrip() const
{
    return HasActorBegunPlay() && !ActiveTrip && Mission->GetWaitingSignal() == TEXT("RequestFloor");
}

TSharedRef<FLiftTrip> AAngelIOExampleLift::MoveTo(int32 Floor, float TravelSpeed, FName Target)
{
    if (Target.IsNone()) Target = Address->GetTargetName();
    auto Trip = MakeShared<FLiftTrip>(GetWorld(), Target.IsNone() ? GetName() : Target.ToString(), Floor, TravelSpeed, Mission);
    const TCHAR* Rejection = nullptr;
    if (!HasAuthority() || GetNetMode() == NM_Client) Rejection = TEXT("NoAuthority");
    else if (ActiveTrip || Mission->GetWaitingSignal() == TEXT("MotionFinished")) Rejection = TEXT("LiftBusy");
    else if (Floor < 1 || Floor > 8) Rejection = TEXT("InvalidFloor");
    else if (!FMath::IsFinite(TravelSpeed) || TravelSpeed <= 0) Rejection = TEXT("InvalidSpeed");
    else if (Mission->GetWaitingSignal() != TEXT("RequestFloor")) Rejection = TEXT("NotReady");
    if (Rejection)
    {
        Trip->Finish(ELiftTripStatus::Rejected, Rejection);
        return Trip;
    }
    FAngelIOValue Override;
    Override.Type = EAngelIOType::Float;
    Override.Float = TravelSpeed;
    if (!Mission->SetInput(TEXT("CommandSpeed"), Override))
    {
        Trip->Finish(ELiftTripStatus::Rejected, TEXT("MissingSpeedInput"));
        return Trip;
    }
    ActiveTrip = Trip;
    RequestFloor(Floor);
    Override.Float = 0;
    Mission->SetInput(TEXT("CommandSpeed"), Override);
    if (ActiveTrip == Trip && Mission->GetWaitingSignal() != TEXT("MotionFinished"))
    {
        ActiveTrip.Reset();
        Trip->Finish(ELiftTripStatus::Rejected, TEXT("RequestNotStarted"));
    }
    return Trip;
}

void AAngelIOExampleLift::EndPlay(const EEndPlayReason::Type Reason)
{
    // Streaming can start this same UObject again before garbage collection.
    ++PlayGeneration;
    Mission->OnSignal.RemoveDynamic(this, &AAngelIOExampleLift::HandleSignal);
    Mission->OnPublicStateChanged.RemoveDynamic(this, &AAngelIOExampleLift::CheckMotionState);
    SetActorTickEnabled(false);
    if (ActiveTrip) ActiveTrip->Finish(ELiftTripStatus::Cancelled,
        Reason == EEndPlayReason::RemovedFromWorld ? TEXT("TargetUnloaded") : TEXT("TargetEndPlay"));
    ActiveTrip.Reset();
    Super::EndPlay(Reason);
}

void AAngelIOExampleLift::CheckMotionState()
{
    if (HasAuthority() && IsActorTickEnabled() && Mission->GetWaitingSignal() != TEXT("MotionFinished"))
    {
        SetActorTickEnabled(false);
        if (ActiveTrip) ActiveTrip->Finish(ELiftTripStatus::Cancelled, TEXT("MissionInterrupted"));
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
            ActiveTrip->Finish(bArrived ? ELiftTripStatus::Arrived : ELiftTripStatus::Cancelled,
                bArrived ? TEXT("") : TEXT("UnexpectedArrival"));
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
