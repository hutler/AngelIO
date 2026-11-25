#pragma once

#include "GameFramework/Actor.h"
#include "AngelIOExampleLift.generated.h"

class UAngelIOScriptComponent;
struct FLiftTrip;

UCLASS()
class AAngelIOExampleLift : public AActor
{
    GENERATED_BODY()
public:
    AAngelIOExampleLift();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> Mission;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> Address;

    UPROPERTY(EditInstanceOnly, Category="Demo")
    bool bRequestOnStart = true;

    // Restore the last completed landing from this host's checkpoint slot.
    UPROPERTY(EditInstanceOnly, Category="Demo")
    bool bPersistLanding = true;

    UPROPERTY(EditInstanceOnly, Category="Demo")
    int32 RequestedFloor = 2;

    UFUNCTION(BlueprintCallable, Category="Demo")
    bool RequestFloor(int32 Floor);
    bool IsReadyForTrip() const;
    TSharedRef<FLiftTrip> MoveTo(int32 Floor, float TravelSpeed, FName Target = NAME_None, FGuid CheckpointStepId = {});
    bool HasSavedArrival(int32 Floor, FGuid CheckpointStepId) const;
    uint64 GetPlayGeneration() const { return PlayGeneration; }

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;

private:
    UFUNCTION()
    void HandleSignal(FString Name, int32 Value);

    UFUNCTION()
    void CheckMotionState();
    bool AtDestination() const;
    void FinishMotion();
    bool LoadLanding(TOptional<int32>& Floor, FString& Error);

    TSharedPtr<FLiftTrip> ActiveTrip;
    FGuid SavedLandingStepId;
    int32 SavedLandingFloor = 0;
    uint64 PlayGeneration = 0;
    TOptional<FVector> StartPositionInLevel;

    double PlacementZ = 0;
    double OriginZ = 0;
    double DestinationZ = 0;
    double StartZ = 0;
    double ElapsedTravelTime = 0;
    float Speed = 0;
    int32 DestinationFloor = 0;
};
