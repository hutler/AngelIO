#include "AngelIOExampleLift.h"
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
}

void AAngelIOExampleLift::BeginPlay()
{
    Mission->OnSignal.AddDynamic(this, &AAngelIOExampleLift::HandleSignal);
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

void AAngelIOExampleLift::HandleSignal(FString Name, int32 Value)
{
    if (Name == TEXT("Arrived"))
    {
        UE_LOG(LogTemp, Display, TEXT("AngelIO demo: %s arrived at floor %d"), *GetName(), Value);
        return;
    }
    if (Name != TEXT("MoveTo")) return;
    FAngelIOValue TripSpeed;
    if (!Mission->GetOutput(TEXT("TripSpeed"), TripSpeed)) return;
    Speed = FMath::Max(1.0f, TripSpeed.Float);
    DestinationFloor = Value;
    DestinationZ = OriginZ + double(Value) * 200.0;
    if (FMath::IsNearlyEqual(GetActorLocation().Z, DestinationZ))
        Mission->SendSignal(TEXT("MotionFinished"), DestinationFloor);
    else
        SetActorTickEnabled(true);
}

void AAngelIOExampleLift::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    FVector Location = GetActorLocation();
    Location.Z = FMath::FInterpConstantTo(Location.Z, DestinationZ, double(DeltaSeconds), double(Speed));
    SetActorLocation(Location);
    if (FMath::IsNearlyEqual(Location.Z, DestinationZ))
    {
        SetActorTickEnabled(false);
        Mission->SendSignal(TEXT("MotionFinished"), DestinationFloor);
    }
}
