#pragma once

#include "GameFramework/Actor.h"
#include "AngelIOExampleLift.generated.h"

class UAngelIOScriptComponent;

UCLASS()
class AAngelIOExampleLift : public AActor
{
    GENERATED_BODY()
public:
    AAngelIOExampleLift();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Demo")
    TObjectPtr<UAngelIOScriptComponent> Mission;

    UPROPERTY(EditInstanceOnly, Category="Demo")
    bool bRequestOnStart = true;

    UPROPERTY(EditInstanceOnly, Category="Demo")
    int32 RequestedFloor = 2;

    UFUNCTION(BlueprintCallable, Category="Demo")
    bool RequestFloor(int32 Floor);

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

private:
    UFUNCTION()
    void HandleSignal(FString Name, int32 Value);

    double OriginZ = 0;
    double DestinationZ = 0;
    float Speed = 0;
    int32 DestinationFloor = 0;
};
