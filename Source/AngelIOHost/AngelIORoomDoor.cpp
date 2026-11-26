#include "AngelIORoomDoor.h"
#include "AngelIOScriptComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

void FDoorOpen::Finish(bool bSuccess, const FString& Reason)
{
    if (IsComplete()) return;
    bOpened = bSuccess;
    MarkComplete(bOpened ? TEXT("Opened") : TEXT("Cancelled"), Reason);
}

AAngelIORoomDoor::AAngelIORoomDoor()
{
    bReplicates = true;
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Frame")));
    Leaf = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Leaf"));
    Leaf->SetupAttachment(RootComponent);
    Leaf->SetMobility(EComponentMobility::Movable);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Leaf->SetStaticMesh(Cube.Object);
    Leaf->SetRelativeScale3D(FVector(1.8, 0.20, 2.2));
    ApplyOpening();
    auto* Address = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("Address"));
    Address->bStartOnBeginPlay = false;
    Address->SetTargetName(TEXT("RoomDoor"));
}

void AAngelIORoomDoor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AAngelIORoomDoor, OpenFraction);
}

void AAngelIORoomDoor::BeginPlay()
{
    if (HasAuthority() && GetNetMode() != NM_Client) OpenFraction = 0.f;
    ApplyOpening();
    Super::BeginPlay();
}

TSharedRef<FDoorOpen> AAngelIORoomDoor::Open(FName Target)
{
    if (Opening) return Opening.ToSharedRef();
    auto Request = MakeShared<FDoorOpen>(GetWorld(), Target, this);
    if (!HasAuthority() || GetNetMode() == NM_Client)
        Request->Finish(false, TEXT("NoAuthority"));
    else if (!HasActorBegunPlay())
        Request->Finish(false, TEXT("NotReady"));
    else if (IsOpen())
        Request->Finish(true, TEXT("AlreadyOpen"));
    else
    {
        Opening = Request;
        SetActorTickEnabled(true);
    }
    return Request;
}

void AAngelIORoomDoor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Opening) return;
    constexpr float OpenDuration = 0.6f;
    OpenFraction = FMath::Min(1.f, OpenFraction + DeltaSeconds / OpenDuration);
    ApplyOpening();
    if (IsOpen())
    {
        Opening->Finish(true);
        Opening.Reset();
        SetActorTickEnabled(false);
        ForceNetUpdate();
    }
}

void AAngelIORoomDoor::ApplyOpening()
{
    Leaf->SetRelativeLocation(FVector(0, 0, 110.f + 230.f * OpenFraction));
}

void AAngelIORoomDoor::EndPlay(const EEndPlayReason::Type Reason)
{
    ++PlayGeneration;
    if (Opening)
    {
        Opening->Finish(false, Reason == EEndPlayReason::RemovedFromWorld ? TEXT("TargetUnloaded") : TEXT("TargetEndPlay"));
        Opening.Reset();
    }
    SetActorTickEnabled(false);
    Super::EndPlay(Reason);
}
