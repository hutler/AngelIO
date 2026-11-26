#include "AngelIORoomDoor.h"
#include "AngelIOScriptComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    const FString DoorStateKey = TEXT("AngelIOHost.OpenDoor.v1");
}

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
    Address = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("Address"));
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
    if (HasAuthority() && GetNetMode() != NM_Client)
    {
        OpenFraction = 0.f;
        bStateAvailable = true;
        if (bPersistOpen)
        {
            TArray<uint8> State;
            FString Error;
            bStateAvailable = Address->LoadComponentState(DoorStateKey, State, Error) &&
                (State.IsEmpty() || (State.Num() == 1 && State[0] == 1));
            if (!bStateAvailable)
            {
                UE_LOG(LogTemp, Error, TEXT("%s cannot restore its door state: %s"), *GetPathName(), *Error);
            }
            else if (!State.IsEmpty())
                OpenFraction = 1.f;
        }
    }
    ApplyOpening();
    Super::BeginPlay();
}

TSharedRef<FDoorOpen> AAngelIORoomDoor::Open(FName Target)
{
    if (Opening) return Opening.ToSharedRef();
    auto Request = MakeShared<FDoorOpen>(GetWorld(), Target, this);
    if (!HasAuthority() || GetNetMode() == NM_Client)
        Request->Finish(false, TEXT("NoAuthority"));
    else if (!bStateAvailable)
        Request->Finish(false, TEXT("DoorStateUnavailable"));
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
        FString Error;
        const uint8 Opened = 1;
        if (bPersistOpen && !Address->SaveComponentState(DoorStateKey, MakeArrayView(&Opened, 1), Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s cannot save its door state: %s"), *GetPathName(), *Error);
            OpenFraction = 0.f;
            ApplyOpening();
            Opening->Finish(false, TEXT("DoorSaveFailed"));
        }
        else Opening->Finish(true);
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
