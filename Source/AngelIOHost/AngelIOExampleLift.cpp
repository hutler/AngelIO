#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIOScriptComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/LevelStreaming.h"
#include "LevelUtils.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
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

namespace
{
    const FString LandingKey = TEXT("AngelIOHost.LiftLanding.v1");
}

bool AAngelIOExampleLift::LoadLanding(TOptional<int32>& Floor, FString& Error)
{
    TArray<uint8> Bytes;
    if (!Mission->LoadComponentState(LandingKey, Bytes, Error)) return false;
    if (Bytes.IsEmpty()) return true;
    if (Bytes.Num() != sizeof(int32) + sizeof(double))
    {
        Error = TEXT("Invalid saved lift landing size");
        return false;
    }
    FMemoryReader Reader(Bytes);
    int32 LandedFloor = 0;
    double OffsetZ = 0;
    Reader << LandedFloor << OffsetZ;
    if (Reader.IsError() || !Reader.AtEnd() || !FMath::IsFinite(OffsetZ))
    {
        Error = TEXT("Invalid saved lift landing");
        return false;
    }
    FVector Location = GetActorLocation();
    Location.Z = PlacementZ + OffsetZ;
    SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
    Floor = LandedFloor;
    return true;
}

void AAngelIOExampleLift::BeginPlay()
{
    const bool bServer = HasAuthority() && GetNetMode() != NM_Client;
    TOptional<int32> RestoredFloor;
    if (bServer)
    {
        if (const auto* Stream = FLevelUtils::FindStreamingLevel(GetLevel()))
        {
            if (StartPositionInLevel.IsSet())
                SetActorLocation(Stream->LevelTransform.TransformPosition(*StartPositionInLevel), false, nullptr, ETeleportType::TeleportPhysics);
            else
                StartPositionInLevel = Stream->LevelTransform.InverseTransformPosition(GetActorLocation());
        }
        PlacementZ = GetActorLocation().Z;
        FString Error;
        if (bPersistLanding && !LoadLanding(RestoredFloor, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s cannot restore its landing: %s"), *GetPathName(), *Error);
            TGuardValue<bool> DisableStartup(Mission->bStartOnBeginPlay, false);
            Super::BeginPlay();
            return;
        }
    }
    Mission->OnSignal.AddDynamic(this, &AAngelIOExampleLift::HandleSignal);
    Mission->OnPublicStateChanged.AddDynamic(this, &AAngelIOExampleLift::CheckMotionState);
    if (RestoredFloor.IsSet())
    {
        // Seed the executor before it publishes its first replicated LastFloor.
        const FString Input = TEXT("StartingFloor");
        TOptional<FAngelIOValue> Authored;
        if (const auto* Value = Mission->InitialInputs.Find(Input)) Authored = *Value;
        FAngelIOValue FloorInput;
        FloorInput.Integer = *RestoredFloor;
        Mission->InitialInputs.Add(Input, FloorInput);
        Super::BeginPlay();
        if (Authored) Mission->InitialInputs.Add(Input, *Authored);
        else Mission->InitialInputs.Remove(Input);
    }
    else Super::BeginPlay();
    if (!bServer) return;
    FAngelIOValue Floor;
    if (Mission->GetOutput(TEXT("LastFloor"), Floor))
        OriginZ = GetActorLocation().Z - double(Floor.Integer) * 200.0;
    if (RestoredFloor.IsSet()) ForceNetUpdate();
    else if (bRequestOnStart) RequestFloor(RequestedFloor);
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
        FinishMotion();
    else
        SetActorTickEnabled(true);
}

bool AAngelIOExampleLift::AtDestination() const
{
    // Unreal may coalesce a sub-millimetre transform update at the end of a trip.
    return FMath::IsNearlyEqual(GetActorLocation().Z, DestinationZ, 0.01);
}

void AAngelIOExampleLift::FinishMotion()
{
    // Commit only a physical arrival, before the VM publishes Arrived or wakes a caller.
    if (bPersistLanding)
    {
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        double OffsetZ = DestinationZ - PlacementZ;
        Writer << DestinationFloor << OffsetZ;
        FString Error;
        if (!Mission->SaveComponentState(LandingKey, Bytes, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s cannot commit its landing: %s"), *GetPathName(), *Error);
            if (ActiveTrip) ActiveTrip->Finish(ELiftTripStatus::Cancelled, TEXT("LandingSaveFailed"));
            ActiveTrip.Reset();
            Mission->StopScript();
            return;
        }
    }
    Mission->SendSignal(TEXT("MotionFinished"), DestinationFloor);
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
        FinishMotion();
    }
}
