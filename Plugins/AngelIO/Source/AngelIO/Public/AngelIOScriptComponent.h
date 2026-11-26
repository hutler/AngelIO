#pragma once

#include "Components/ActorComponent.h"
#include "AngelIOProgram.h"
#include "AngelIOScriptComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAngelIOSignal, FString, SignalName, int32, Value);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAngelIOStateChanged);

enum class EAngelIOStateChange { Saved, Restored };

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
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO", meta=(GetOptions="GetMissionScripts"))
    FString Script;

    UFUNCTION()
    TArray<FString> GetMissionScripts() const;

    // A mapped name resolves only to endpoints placed in this component's level instance.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AngelIO")
    TMap<FName, FName> RoomTargets;

    // Set on a placed component before publishing a persistent mission. PIE duplicates preserve it.
    UPROPERTY(EditInstanceOnly, Category="AngelIO|Persistence")
    FGuid PersistenceId;
    UPROPERTY(EditInstanceOnly, Category="AngelIO|Persistence")
    FName PersistenceContract;
    // Former actor.component object paths relative to this room's level.
    UPROPERTY(EditInstanceOnly, Category="AngelIO|Persistence")
    TArray<FString> FormerPlacementPaths;
    UPROPERTY(EditInstanceOnly, Category="AngelIO|Persistence")
    TArray<FString> FormerScripts;

#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="AngelIO|Persistence")
    void AssignPersistenceId();
    void PostEditImport() override;
#endif

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
    bool SendSignalFrom(const FString& Name, int32 Value, const UObject& Sender);

    UFUNCTION(BlueprintPure, Category="AngelIO")
    FString GetWaitingSignal() const;
    // Native gameplay state shares the mission slot and placed component identity. An empty load means no saved state.
    bool LoadComponentState(const FString& Key, TArray<uint8>& Data, FString& Error) const;
    bool SaveComponentState(const FString& Key, TConstArrayView<uint8> Data, FString& Error) const;
    // Report a display value after the native owner commits a record or applies a valid restore.
    void ReportPersistedInteger(EAngelIOStateChange Change, const FString& Key, FName Target, const FString& Field, int32 Value);
#if WITH_EDITOR
    FAngelIOMissionInspection InspectMission() const;
    bool RestartValidated(FString& Result);
    bool ResolveRunningSource(FString& Path, int32& Line) const;
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;

private:
    friend class UAngelIOTargetRegistry;
    void ReceiveNamedInput(const FString& Name, int32 Value, FGuid SignalId);
    bool DeliverSignal(const FString& Name, int32 Value, FGuid SignalId);
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
#if WITH_EDITOR
    TMap<FGuid, FAngelIOInspectedInteger> PersistedIntegers;
#endif
    void FlushEvents();
    void DispatchPendingEvents();
    void ReportErrors() const;
};
