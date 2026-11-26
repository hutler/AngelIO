#pragma once

#include "AngelIOScriptAPI.h"
#include "GameFramework/Actor.h"
#include "AngelIORoomDoor.generated.h"

class UStaticMeshComponent;

struct FDoorOpen : FAngelIOOperation
{
    FDoorOpen(UWorld* World, FName Target, const UObject* Door)
        : FAngelIOOperation(World, TEXT("Door.Open"), Target.ToString(), nullptr, Door) {}

    void Finish(bool bSuccess, const FString& Reason = {});
    bool bOpened = false;
};

UCLASS()
class AAngelIORoomDoor : public AActor
{
    GENERATED_BODY()
public:
    AAngelIORoomDoor();
    TSharedRef<FDoorOpen> Open(FName Target);
    uint64 GetPlayGeneration() const { return PlayGeneration; }
    bool IsOpen() const { return OpenFraction == 1.f; }

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Door")
    TObjectPtr<UStaticMeshComponent> Leaf;

private:
    void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type Reason) override;
    void Tick(float DeltaSeconds) override;

    UFUNCTION()
    void ApplyOpening();

    UPROPERTY(ReplicatedUsing=ApplyOpening)
    float OpenFraction = 0.f;
    uint64 PlayGeneration = 0;
    TSharedPtr<FDoorOpen> Opening;
};
