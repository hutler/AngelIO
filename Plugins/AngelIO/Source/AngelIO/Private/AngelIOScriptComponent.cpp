#include "AngelIOScriptComponent.h"
#include "AngelIOCheckpointSubsystem.h"
#include "AngelIOTargetRegistry.h"
#include "AngelIOTraceSubsystem.h"
#include "AngelIOScriptAPI.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/AngelIOStartupProbe.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAngelIO, Log, All);

UAngelIOScriptComponent::UAngelIOScriptComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

TArray<FString> UAngelIOScriptComponent::GetMissionScripts() const
{
    TArray<FString> Files;
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("AngelIO")) + TEXT("/");
    IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.as"), true, false);
    for (FString& File : Files) FPaths::MakePathRelativeTo(File, *Root);
    Files.Sort();
    return Files;
}

#if WITH_EDITOR
void UAngelIOScriptComponent::AssignPersistenceId()
{
    if (!IsTemplate() && !PersistenceId.IsValid())
    {
        Modify();
        PersistenceId = FGuid::NewGuid();
    }
}

void UAngelIOScriptComponent::PostEditImport()
{
    Super::PostEditImport();
    if (!IsTemplate() && PersistenceId.IsValid())
    {
        PersistenceId = FGuid::NewGuid();
        FormerPlacementPaths.Reset();
    }
}
#endif

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
    ReleaseProgram();
    PublicState = {};
#if WITH_EDITOR
    PersistedIntegers.Reset();
#endif
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

bool UAngelIOScriptComponent::DeliverSignal(const FString& Name, int32 Value, FGuid SignalId)
{
    const FGuid Recipient = Program ? Program->GetInstanceId() : FGuid();
    const FString Path = SignalId.IsValid() ? GetPathName(GetWorld()) : FString();
    const bool bAccepted = SendSignal(Name, Value);
    if (SignalId.IsValid())
        if (auto* Trace = GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>())
            Trace->SignalRecipient(SignalId, Path, Recipient, bAccepted ? TEXT("Consumed") : TEXT("NoScriptWait"));
    return bAccepted;
}

bool UAngelIOScriptComponent::SendSignalFrom(const FString& Name, int32 Value, const UObject& Sender)
{
    if (!CanRunMission()) return false;
    FGuid Id;
    if (auto* Trace = GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>())
    {
        auto Caller = MakeShared<FJsonObject>();
        Caller->SetStringField(TEXT("kind"), TEXT("native"));
        Caller->SetStringField(TEXT("component"), Sender.GetPathName(GetWorld()));
        Id = Trace->SignalSent(Name, Value, GetPathName(GetWorld()), 1, Caller);
    }
    return DeliverSignal(Name, Value, Id);
}

void UAngelIOScriptComponent::ReceiveNamedInput(const FString& Name, int32 Value, FGuid SignalId)
{
    DeliverSignal(Name, Value, SignalId);
    if (HasBegunPlay()) OnNamedInput.Broadcast(Name, Value);
}

bool UAngelIOScriptComponent::StartScript()
{
    if (!CanRunMission()) return false;
    Pending.Reset();
    ReleaseProgram();
    Program = MakeShared<FAngelIOProgram>();
    auto Running = Program;
    const bool bStarted = Running->CompileFile(Script, GetWorld()) && Running->Start(InitialInputs, GetWorld(), this);
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
    ReleaseProgram();
    PublishState();
}

void UAngelIOScriptComponent::ReleaseProgram()
{
    // A native callback can stop or replace the VM while Run still holds it on the stack.
    if (Program) Program->DetachComponent();
    Program.Reset();
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

bool UAngelIOScriptComponent::LoadComponentState(const FString& Key, TArray<uint8>& Data, FString& Error) const
{
    Data.Reset();
    if (CanRunMission() && GetOwner()->GetLevel())
        if (auto* Store = GetWorld()->GetSubsystem<UAngelIOCheckpointSubsystem>())
            return Store->LoadState(*this, Key, Data, Error);
    Error = TEXT("Loading component state requires an authoritative game world");
    return false;
}

bool UAngelIOScriptComponent::SaveComponentState(const FString& Key, TConstArrayView<uint8> Data, FString& Error) const
{
    if (CanRunMission() && GetOwner()->GetLevel())
        if (auto* Store = GetWorld()->GetSubsystem<UAngelIOCheckpointSubsystem>())
            return Store->SaveState(*this, Key, Data, Error);
    Error = TEXT("Saving component state requires an authoritative game world");
    return false;
}

void UAngelIOScriptComponent::ReportPersistedInteger(EAngelIOStateChange Change, const FString& Key, FName Target,
    const FString& Field, int32 Value)
{
    if (!CanRunMission()) return;
    auto* Trace = GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>();
    if (!Trace) return;
    const FGuid Id = GetWorld()->GetSubsystem<UAngelIOCheckpointSubsystem>()->StateId(*this, Key);
    const bool bRestored = Change == EAngelIOStateChange::Restored;
    auto Event = Trace->Append({}, bRestored ? TEXT("stateRestored") : TEXT("stateSaved"), this);
    Event->SetStringField(TEXT("stateId"), Id.ToString(EGuidFormats::DigitsWithHyphensLower));
    Event->SetStringField(TEXT("target"), Target.IsNone() ? GetOwner()->GetName() : Target.ToString());
    Event->SetStringField(TEXT("component"), GetPathName());
    Event->SetStringField(TEXT("field"), Field);
    Event->SetNumberField(TEXT("value"), Value);
#if WITH_EDITOR
    auto& Info = PersistedIntegers.FindOrAdd(Id);
    Info.Field = Field;
    Info.Saved = Value;
    if (bRestored) Info.Restored = Value;
#endif
}

FString UAngelIOScriptComponent::GetWaitingSignal() const
{
    if (CanRunMission()) return Program ? Program->WaitingFor() : FString();
    return PublicState.WaitingFor;
}

void UAngelIOScriptComponent::FlushEvents()
{
    Pending.Append(Program->TakeEvents());
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
    if (auto* Peak = FAngelIOStartupProbe::QueuePeak) *Peak = FMath::Max(*Peak, Pending.Num());
#endif
    if (bDispatching) return;
    DispatchPendingEvents();
    PublishState();
}

#if WITH_EDITOR
bool UAngelIOScriptComponent::ResolveRunningSource(FString& Path, int32& Line) const
{
    return Program && Program->ResolveRunningSource(Path, Line);
}

bool UAngelIOScriptComponent::RestartValidated(FString& Result)
{
    if (!CanRunMission() || bDispatching || !HasBegunPlay()) { Result = TEXT("Select a live authority mission between dispatches."); return false; }
    const auto Before = InspectMission();
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("oldRunId"), Before.Instance.ToString(EGuidFormats::DigitsWithHyphensLower));
    Args->SetStringField(TEXT("oldRevision"), Before.SourceRevision);
    Args->SetStringField(TEXT("waitDisposition"), Before.Operation.IsValid() ? TEXT("detach reader; producer continues") : TEXT("release script waits"));
    if (Before.Operation.IsValid()) Args->SetStringField(TEXT("oldOperationId"), Before.Operation.ToString(EGuidFormats::DigitsWithHyphensLower));
    struct FRestart : FAngelIOOperation { using FAngelIOOperation::FAngelIOOperation; using FAngelIOOperation::MarkComplete; };
    FRestart Restart(GetWorld(), TEXT("Mission.Restart"), Script, Args, this);
    auto Candidate = MakeShared<FAngelIOProgram>();
    if (!Candidate->CompileFile(Script, GetWorld()))
    {
        Result.Reset();
        for (const auto& D : Candidate->Diagnostics()) Result += FString::Printf(TEXT("%s:%d %s\n"), *Script, D.Line, *D.Message);
        Restart.MarkComplete(TEXT("Rejected"), Result);
        return false;
    }
    for (const auto& Entry : InitialInputs)
        if (!Candidate->Ports().ContainsByPredicate([&](const auto& P){ return P.bInput && P.Name == Entry.Key && P.Type == Entry.Value.Type; }))
        {
            Result = TEXT("Invalid initial input: ") + Entry.Key;
            Restart.MarkComplete(TEXT("Rejected"), Result);
            return false;
        }
    Pending.Reset();
    ReleaseProgram();
    Program = Candidate;
    const bool Started = Candidate->Start(InitialInputs, GetWorld(), this);
    if (Program != Candidate) { Result = TEXT("Mission replaced during startup"); Restart.MarkComplete(TEXT("Replaced"), Result); return false; }
    if (Started) FlushEvents();
    else { ReportErrors(); PublishState(); }
    const bool Success = Started && Program == Candidate && Candidate->State() != EAngelIOState::Failed;
    Result = Success ? TEXT("Restarted from the mission entry point.") : TEXT("New mission failed during startup.");
    Restart.MarkComplete(Success ? TEXT("Restarted") : TEXT("Failed"), Result);
    return Success;
}

FAngelIOMissionInspection UAngelIOScriptComponent::InspectMission() const
{
    auto Info = Program ? Program->InspectMission() : FAngelIOMissionInspection{};
    Info.PersistedIntegers = PersistedIntegers;
    if (Info.Script.IsEmpty()) Info.Script = Script;
    if (CanRunMission() && GetOwner()->GetLevel())
        if (auto* Store = GetWorld()->GetSubsystem<UAngelIOCheckpointSubsystem>())
        {
            UAngelIOCheckpointSubsystem::FCheckpoint Record;
            FString Error;
            if (Store->Load(*this, Info.Script, Record, Error, true) && Record.Version > 0)
            {
                Info.CheckpointId = Record.Id;
                Info.SavedStage = FString(Record.Stage);
                Info.SavedVersion = Record.Version;
            }
        }
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
        if (Event.bNotifySelf)
        {
            SendSignal(Event.Name, Event.Value);
            continue;
        }
        if (!Event.Target.IsNone())
        {
            if (auto* Registry = GetWorld()->GetSubsystem<UAngelIOTargetRegistry>())
            {
                const FName* Bound = RoomTargets.Find(Event.Target);
                Registry->SendToInLevel(Bound ? *Bound : Event.Target, Event.Name, Event.Value, Bound ? GetOwner()->GetLevel() : nullptr, Event.Caller);
            }
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
    {
        if (auto* Trace = D.Severity == TEXT("error") ? GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>() : nullptr)
        {
            auto Site = MakeShared<FJsonObject>();
            Site->SetStringField(TEXT("script"), Script);
            Site->SetNumberField(TEXT("line"), D.Line);
            Site->SetStringField(TEXT("component"), GetPathName(GetWorld()));
            const FGuid Instance = Program->GetInstanceId();
            if (Instance.IsValid()) Site->SetStringField(TEXT("instanceId"), Instance.ToString(EGuidFormats::DigitsWithHyphensLower));
            Trace->RecordDecision(TEXT("Mission.Error"), Script, D.Message, Site, this);
        }
        UE_LOG(LogAngelIO, Warning, TEXT("%s(%d,%d) %s: %s"), *Script, D.Line, D.Column, *D.Severity, *D.Message);
    }
}
