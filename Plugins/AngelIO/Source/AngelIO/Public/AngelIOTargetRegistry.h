#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "AngelIOTargetRegistry.generated.h"

class UAngelIOScriptComponent;
class AActor;

UCLASS()
class ANGELIO_API UAngelIOTargetRegistry : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category="AngelIO")
    TArray<UAngelIOScriptComponent*> FindTargets(FName TargetName) const;
    // Multiple matching endpoints on the same owner still identify one actor.
    AActor* FindUniqueActor(FName TargetName, UClass* ActorClass) const;

    // Returns the number of live recipients selected at send time.
    UFUNCTION(BlueprintCallable, Category="AngelIO")
    int32 SendTo(FName TargetName, const FString& InputSignal, int32 Value);

private:
    friend class UAngelIOScriptComponent;
    void Register(FName Name, UAngelIOScriptComponent* Target);
    void Unregister(FName Name, UAngelIOScriptComponent* Target);
    struct FDelivery
    {
        TArray<TWeakObjectPtr<UAngelIOScriptComponent>> Recipients;
        FString Input;
        int32 Value;
    };
    TMap<FName, TArray<TWeakObjectPtr<UAngelIOScriptComponent>>> Groups;
    TArray<FDelivery> Queue;
    bool bDelivering = false;
};
