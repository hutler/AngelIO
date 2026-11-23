#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogAngelIO, Log, All);

UAngelIOScriptComponent::UAngelIOScriptComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UAngelIOScriptComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UAngelIOScriptComponent, PublicState);
    DOREPLIFETIME(UAngelIOScriptComponent, TargetName);
}

bool UAngelIOScriptComponent::CanRunMission() const
{
    // A client-only actor may have ROLE_Authority in a client world.
    return GetOwner() && GetOwner()->HasAuthority() && !IsNetMode(NM_Client);
}

void UAngelIOScriptComponent::BeginPlay()
{
    Super::BeginPlay();
    if (CanRunMission())
        if (auto* Registry = GetWorld()->GetSubsystem<UAngelIOTargetRegistry>())
            Registry->Register(TargetName, this);
    if (bStartOnBeginPlay) StartScript();
}

void UAngelIOScriptComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (auto* Registry = GetWorld()->GetSubsystem<UAngelIOTargetRegistry>())
        Registry->Unregister(TargetName, this);
    Pending.Reset();
    Program.Reset();
    PublicState = {};
    Super::EndPlay(Reason);
}

void UAngelIOScriptComponent::SetTargetName(FName Name)
{
    if (TargetName == Name) return;
    if (HasBegunPlay())
    {
        if (!CanRunMission()) return;
        auto* Registry = GetWorld()->GetSubsystem<UAngelIOTargetRegistry>();
        if (Registry)
        {
            Registry->Unregister(TargetName, this);
            Registry->Register(Name, this);
        }
        if (GetOwner()->GetIsReplicated()) GetOwner()->FlushNetDormancy();
    }
    TargetName = Name;
}

void UAngelIOScriptComponent::ReceiveNamedInput(const FString& Name, int32 Value)
{
    SendSignal(Name, Value);
    if (HasBegunPlay()) OnNamedInput.Broadcast(Name, Value);
}

bool UAngelIOScriptComponent::StartScript()
{
    if (!CanRunMission()) return false;
    Pending.Reset();
    Program = MakeShared<FAngelIOProgram>();
    auto Running = Program;
    const bool bStarted = Running->CompileFile(Script) && Running->Start(InitialInputs, GetWorld(), this);
    if (Running != Program) return false;
    if (!bStarted)
    {
        ReportErrors();
        PublishState();
        return false;
    }
    FlushEvents();
    return Program.IsValid() && Program->State() != EAngelIOState::Failed;
}

void UAngelIOScriptComponent::StopScript()
{
    if (!CanRunMission()) return;
    Pending.Reset();
    Program.Reset();
    PublishState();
}

bool UAngelIOScriptComponent::SetInput(const FString& Name, FAngelIOValue Value)
{
    return CanRunMission() && Program && Program->SetInput(Name, Value);
}

bool UAngelIOScriptComponent::GetOutput(const FString& Name, FAngelIOValue& Value) const
{
    if (CanRunMission()) return Program && Program->GetOutput(Name, Value);
    for (const auto& Output : PublicState.Outputs)
    {
        if (Output.Name.Equals(Name, ESearchCase::CaseSensitive))
        {
            Value = Output.Value;
            return true;
        }
    }
    return false;
}

bool UAngelIOScriptComponent::SendSignal(const FString& Name, int32 Value)
{
    if (!CanRunMission() || !Program || Program->State() != EAngelIOState::Waiting) return false;
    auto Running = Program;
    const bool bDelivered = Running->Signal(Name, Value);
    if (Running != Program) return bDelivered;
    if (!bDelivered)
    {
        if (Program->State() == EAngelIOState::Failed)
        {
            ReportErrors();
            PublishState();
        }
        return false;
    }
    FlushEvents();
    return true;
}

FString UAngelIOScriptComponent::GetWaitingSignal() const
{
    if (CanRunMission()) return Program ? Program->WaitingFor() : FString();
    return PublicState.WaitingFor;
}

void UAngelIOScriptComponent::FlushEvents()
{
    Pending.Append(Program->TakeEvents());
    if (bDispatching) return;
    DispatchPendingEvents();
    PublishState();
}

#if WITH_EDITOR
FAngelIOMissionInspection UAngelIOScriptComponent::InspectMission() const
{
    auto Info = Program ? Program->InspectMission() : FAngelIOMissionInspection{};
    if (Info.Script.IsEmpty()) Info.Script = Script;
    return Info;
}
#endif

void UAngelIOScriptComponent::DispatchPendingEvents()
{
    TGuardValue<bool> Guard(bDispatching, true);
    // Script reaches its next wait before observers can respond synchronously.
    for (int32 Count = 0; !Pending.IsEmpty(); ++Count)
    {
        if (Count >= 1024)
        {
            UE_LOG(LogAngelIO, Error, TEXT("%s: signal feedback loop"), *Script);
            StopScript();
            break;
        }
        const FAngelIOEvent Event = MoveTemp(Pending[0]);
        Pending.RemoveAt(0, EAllowShrinking::No);
        if (!Event.Target.IsNone())
        {
            if (auto* Registry = GetWorld()->GetSubsystem<UAngelIOTargetRegistry>())
                Registry->SendTo(Event.Target, Event.Name, Event.Value);
            continue;
        }
        if (PresentationSignals.ContainsByPredicate([&](const FString& Name)
            { return Name.Equals(Event.Name, ESearchCase::CaseSensitive); }))
        {
            MulticastPresentationSignal(Event.Name, Event.Value);
        }
        OnSignal.Broadcast(Event.Name, Event.Value);
    }
}

void UAngelIOScriptComponent::MulticastPresentationSignal_Implementation(const FString& Name, int32 Value)
{
    OnPresentationSignal.Broadcast(Name, Value);
}

void UAngelIOScriptComponent::PublishState()
{
    if (bDispatching) return;
    SetComponentTickEnabled(Program && Program->IsWaitingOnOperation());
    FAngelIOPublicState Next;
    if (Program)
    {
        Next.WaitingFor = Program->WaitingFor();
        for (const auto& Port : Program->Ports())
        {
            FAngelIOValue Value;
            if (!Port.bInput && Program->GetOutput(Port.Name, Value))
                Next.Outputs.Add({Port.Name, Value});
        }
    }
    if (FAngelIOPublicState::StaticStruct()->CompareScriptStruct(&PublicState, &Next, 0)) return;
    AActor* Owner = GetOwner();
    if (Owner->GetIsReplicated()) Owner->FlushNetDormancy();
    PublicState = MoveTemp(Next);
    if (Owner->GetIsReplicated()) Owner->ForceNetUpdate();
    OnPublicStateChanged.Broadcast();
}

void UAngelIOScriptComponent::OnRep_PublicState()
{
    OnPublicStateChanged.Broadcast();
}

void UAngelIOScriptComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    auto Running = Program;
    if (!Running) return;
    const bool bResumed = Running->ResumeCompletedOperation();
    if (Running != Program) return;
    if (bResumed) FlushEvents();
    else if (Running->State() == EAngelIOState::Failed)
    {
        ReportErrors();
        PublishState();
    }
}

void UAngelIOScriptComponent::ReportErrors() const
{
    for (const auto& D : Program->Diagnostics())
        UE_LOG(LogAngelIO, Warning, TEXT("%s(%d,%d) %s: %s"), *Script, D.Line, D.Column, *D.Severity, *D.Message);
}
