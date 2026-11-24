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

    UPROPERTY(EditInstanceOnly, Category="Demo")
    int32 RequestedFloor = 2;

    UFUNCTION(BlueprintCallable, Category="Demo")
    bool RequestFloor(int32 Floor);
    TSharedRef<FLiftTrip> MoveTo(int32 Floor, float TravelSpeed, FName Target = NAME_None);
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

    TSharedPtr<FLiftTrip> ActiveTrip;
    uint64 PlayGeneration = 0;
    TOptional<FVector> StartPositionInLevel;

    double OriginZ = 0;
    double DestinationZ = 0;
    double StartZ = 0;
    double ElapsedTravelTime = 0;
    float Speed = 0;
    int32 DestinationFloor = 0;
};
