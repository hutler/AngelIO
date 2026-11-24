#pragma once

#include "Components/ActorComponent.h"
#include "AngelIOProgram.h"
#include "AngelIOScriptComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAngelIOSignal, FString, SignalName, int32, Value);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAngelIOStateChanged);

USTRUCT()
struct FAngelIOPublicOutput
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() FAngelIOValue Value;
};

USTRUCT()
struct FAngelIOPublicState
{
    GENERATED_BODY()
    UPROPERTY() TArray<FAngelIOPublicOutput> Outputs;
    UPROPERTY() FString WaitingFor;
};

UCLASS(ClassGroup=(AngelIO), meta=(BlueprintSpawnableComponent))
class ANGELIO_API UAngelIOScriptComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UAngelIOScriptComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UFUNCTION(BlueprintCallable, Category="AngelIO")
    void SetTargetName(FName Name);

    UFUNCTION(BlueprintPure, Category="AngelIO")
    FName GetTargetName() const { return TargetName; }

    // Relative to the project's Content/AngelIO, for example Elevator.as.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO")
    FString Script;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO")
    bool bStartOnBeginPlay = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO")
    TMap<FString, FAngelIOValue> InitialInputs;

    UPROPERTY(BlueprintAssignable, Category="AngelIO")
    FAngelIOSignal OnSignal;

    // A named endpoint can also be implemented entirely in Blueprint or C++.
    UPROPERTY(BlueprintAssignable, Category="AngelIO")
    FAngelIOSignal OnNamedInput;

    // Transient effects for currently relevant peers. The payload is self-contained.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AngelIO|Network")
    TArray<FString> PresentationSignals;

    UPROPERTY(BlueprintAssignable, Category="AngelIO|Network")
    FAngelIOSignal OnPresentationSignal;

    UPROPERTY(BlueprintAssignable, Category="AngelIO|Network")
    FAngelIOStateChanged OnPublicStateChanged;

    UFUNCTION(BlueprintCallable, Category="AngelIO")
    bool StartScript();

    UFUNCTION(BlueprintCallable, Category="AngelIO")
    void StopScript();

    UFUNCTION(BlueprintCallable, Category="AngelIO")
    bool SetInput(const FString& Name, FAngelIOValue Value);

    UFUNCTION(BlueprintPure, Category="AngelIO")
    bool GetOutput(const FString& Name, FAngelIOValue& Value) const;

    UFUNCTION(BlueprintCallable, Category="AngelIO")
    bool SendSignal(const FString& Name, int32 Value);

    UFUNCTION(BlueprintPure, Category="AngelIO")
    FString GetWaitingSignal() const;
    // Native gameplay state shares the mission slot and placed component identity. An empty load means no saved state.
    bool LoadComponentState(const FString& Key, TArray<uint8>& Data, FString& Error) const;
    bool SaveComponentState(const FString& Key, TConstArrayView<uint8> Data, FString& Error) const;
#if WITH_EDITOR
    FAngelIOMissionInspection InspectMission() const;
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;

private:
    friend class UAngelIOTargetRegistry;
    void ReceiveNamedInput(const FString& Name, int32 Value);
    UPROPERTY(EditAnywhere, Replicated, Category="AngelIO")
    FName TargetName;

    UPROPERTY(ReplicatedUsing=OnRep_PublicState)
    FAngelIOPublicState PublicState;

    UFUNCTION()
    void OnRep_PublicState();

    UFUNCTION(NetMulticast, Unreliable)
    void MulticastPresentationSignal(const FString& Name, int32 Value);

    bool CanRunMission() const;
    void ReleaseProgram();
    void PublishState();
    TSharedPtr<FAngelIOProgram> Program;
    TArray<FAngelIOEvent> Pending;
    bool bDispatching = false;
    void FlushEvents();
    void DispatchPendingEvents();
    void ReportErrors() const;
};
