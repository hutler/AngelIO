#include "AngelIORoomTerminal.h"
#include "AngelIOScriptComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "UObject/ConstructorHelpers.h"

AAngelIORoomTerminal::AAngelIORoomTerminal()
{
    bReplicates = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Stand")));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto* Pedestal = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Pedestal"));
    Pedestal->SetupAttachment(RootComponent);
    Pedestal->SetStaticMesh(Cube.Object);
    Pedestal->SetRelativeScale3D(FVector(0.36, 0.32, 0.9));
    Pedestal->SetRelativeLocation(FVector(0, 0, 45));
    auto* Console = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Console"));
    Console->SetupAttachment(RootComponent);
    Console->SetStaticMesh(Cube.Object);
    Console->SetRelativeScale3D(FVector(0.56, 0.4, 0.09));
    Console->SetRelativeRotation(FRotator(0, 0, -18));
    Console->SetRelativeLocation(FVector(0, 0, 94));
    Mission = CreateDefaultSubobject<UAngelIOScriptComponent>(TEXT("Mission"));
    Mission->Script = TEXT("Examples/DoorTerminal.as");
    Mission->RoomTargets.Add(TEXT("Door"), TEXT("RoomDoor"));
}

bool AAngelIORoomTerminal::IsInReach(const APawn* Pawn) const
{
    constexpr double UseDistance = 300.0;
    return IsValid(Pawn) && HasActorBegunPlay() && !IsActorBeingDestroyed() && GetWorld() == Pawn->GetWorld() &&
        FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation()) <= FMath::Square(UseDistance);
}
