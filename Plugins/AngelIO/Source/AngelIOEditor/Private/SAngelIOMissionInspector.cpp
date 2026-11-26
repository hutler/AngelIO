#include "SAngelIOMissionInspector.h"
#include "ISourceCodeAccessModule.h"
#include "ISourceCodeAccessor.h"
#include "Widgets/Input/SButton.h"
#include "AngelIOBytecodeCacheStats.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "EngineUtils.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/PlayerController.h"
#include "IPAddress.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

const FName SAngelIOMissionInspector::TabId(TEXT("AngelIO.MissionInspector"));

namespace
{
    TArray<UWorld*> PlayWorlds()
    {
        TArray<UWorld*> Result;
        if (GEngine)
            for (const auto& Context : GEngine->GetWorldContexts())
            {
                UWorld* W = Context.World();
                if (Context.WorldType == EWorldType::PIE && IsValid(W) && W->HasBegunPlay() && !W->bIsTearingDown)
                    Result.Add(W);
            }
        return Result;
    }

    FString WorldName(UWorld* World)
    {
        if (!World) return TEXT("Choose PIE world");
        const ENetMode Mode = World->GetNetMode();
        const TCHAR* Role = Mode == NM_Client ? TEXT("Client") : Mode == NM_ListenServer ? TEXT("Listen server") :
            Mode == NM_Standalone ? TEXT("Standalone") : TEXT("Server");
        if (Mode == NM_Client)
            if (const auto* Context = GEngine->GetWorldContextFromWorld(World))
                return FString::Printf(TEXT("Client (PIE %d)  ·  %s"), Context->PIEInstance, *World->GetName());
        return FString::Printf(TEXT("%s  ·  %s"), Role, *World->GetName());
    }

    FString MissionName(const UAngelIOScriptComponent* Mission)
    {
        if (!Mission || !IsValid(Mission->GetOwner())) return TEXT("Choose mission");
        const auto* Owner = Mission->GetOwner();
        FString Label = Owner->GetActorNameOrLabel();
        if (const auto* PC = Cast<APlayerController>(Owner))
            Label = Owner->GetName() + (PC->IsLocalController() ? TEXT(" (local player)") : TEXT(" (remote player)"));
        else
            for (auto* C : TInlineComponentArray<UAngelIOScriptComponent*>(Owner))
                if (C != Mission && C->Script.IsEmpty() && !C->GetTargetName().IsNone())
                    Label += TEXT(" / ") + C->GetTargetName().ToString();
        return FString::Printf(TEXT("%s  ·  %s  ·  %s"), *Label, *Mission->GetName(), *Mission->Script);
    }

    bool MatchesLocalEndpoint(const FInternetAddr& Bound, const FInternetAddr& Remote, ISocketSubsystem& Sockets)
    {
        if (Bound.GetPort() != Remote.GetPort()) return false;
        if (Bound.CompareEndpoints(Remote)) return true;
        auto Address = Bound.Clone();
        Address->SetAnyAddress();
        Address->SetPort(Bound.GetPort());
        if (!Address->CompareEndpoints(Bound)) return false;
        // An IP driver may bind to all adapters. Only a local destination can be its peer here.
        Address = Remote.Clone();
        Address->SetLoopbackAddress();
        Address->SetPort(Remote.GetPort());
        if (Address->CompareEndpoints(Remote)) return true;
        TArray<TSharedPtr<FInternetAddr>> Adapters;
        Sockets.GetLocalAdapterAddresses(Adapters);
        for (const auto& Adapter : Adapters)
        {
            if (!Adapter) continue;
            auto Candidate = Adapter->Clone();
            Candidate->SetPort(Remote.GetPort());
            if (Candidate->CompareEndpoints(Remote)) return true;
        }
        return false;
    }

    // Resolve through the connected in-process IP driver. Display only the replica's
    // public wait if the host or the network identity isn't available in this editor.
    UAngelIOScriptComponent* ResolveAuthority(UAngelIOScriptComponent* Selected)
    {
        if (Selected->GetNetMode() != NM_Client) return Selected;
        auto* Client = Selected->GetWorld()->GetNetDriver();
        if (!Client || !Client->ServerConnection || Client->IsUsingIrisReplication() || !Client->GetNetGuidCache()) return nullptr;
        auto Endpoint = Client->ServerConnection->GetRemoteAddr();
        auto Local = Client->GetLocalAddr();
        auto* Sockets = Client->GetSocketSubsystem();
        if (!Endpoint || !Local || !Sockets) return nullptr;
        const auto OwnerId = Client->GetNetGuidCache()->GetNetGUID(Selected->GetOwner());
        const auto ComponentId = Client->GetNetGuidCache()->GetNetGUID(Selected);
        for (UWorld* W : PlayWorlds())
        {
            auto* Host = W->GetNetDriver();
            if (W->GetNetMode() != NM_ListenServer || !Host || Host->IsUsingIrisReplication() || !Host->GetNetGuidCache()) continue;
            auto HostAddr = Host->GetLocalAddr();
            if (Host->GetSocketSubsystem() != Sockets || !HostAddr || !MatchesLocalEndpoint(*HostAddr, *Endpoint, *Sockets)) continue;
            for (const auto& Connection : Host->ClientConnections)
            {
                if (!Connection) continue;
                auto Peer = Connection->GetRemoteAddr();
                if (!Peer || !MatchesLocalEndpoint(*Local, *Peer, *Sockets)) continue;
                const auto& Cache = *Host->GetNetGuidCache();
                if (const auto* Entry = Cache.GetCacheObject(ComponentId))
                    if (auto* C = Cast<UAngelIOScriptComponent>(Entry->Object.Get()); C && C->GetWorld() == W) return C;
                AActor* Actor = nullptr;
                if (const auto* Entry = Cache.GetCacheObject(OwnerId)) Actor = Cast<AActor>(Entry->Object.Get());
                if (!Actor && Selected->GetOwner() == Client->ServerConnection->PlayerController)
                    Actor = Connection->PlayerController;
                if (Actor && Actor->GetWorld() == W && Selected->IsNameStableForNetworking())
                    for (auto* C : TInlineComponentArray<UAngelIOScriptComponent*>(Actor))
                        if (C->GetFName() == Selected->GetFName()) return C;
            }
        }
        return nullptr;
    }

    FString Field(const FJsonObject& Json, const TCHAR* Name)
    {
        FString Value;
        Json.TryGetStringField(Name, Value);
        return Value;
    }

    const FJsonObject* Site(const FJsonObject& Event, const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>* Value = nullptr;
        return Event.TryGetObjectField(Key, Value) ? Value->Get() : nullptr;
    }

    FString DescribeEvent(const FJsonObject& Event)
    {
        const FString Type = Field(Event, TEXT("type"));
        FString Detail;
        if (Type == TEXT("call") || Type == TEXT("decision") || Type == TEXT("signalSent"))
        {
            Detail = Field(Event, TEXT("operation")) + TEXT(" → ") + Field(Event, TEXT("target"));
            const TSharedPtr<FJsonObject>* Args = nullptr;
            if (Event.TryGetObjectField(TEXT("arguments"), Args))
            {
                FString Json;
                FJsonSerializer::Serialize(*Args, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
                Detail += TEXT("  ") + Json;
            }
        }
        else if (Type == TEXT("signalRecipient"))
            Detail = Field(Event, TEXT("component"));
        else if (Type == TEXT("complete"))
            Detail = (Field(Event, TEXT("status")) + TEXT("  ") + Field(Event, TEXT("reason"))).TrimEnd();
        else if (Field(Event, TEXT("outcome")) == TEXT("TimedOut"))
            Detail = TEXT("Waiter timed out");
        else if (Type == TEXT("wait"))
            Detail = Event.GetBoolField(TEXT("suspended")) ? TEXT("Suspended") : TEXT("Already complete");
        if (Type == TEXT("decision") || Type.StartsWith(TEXT("signal"))) Detail += TEXT("  ") + Field(Event, TEXT("reason"));
        const auto* Source = Site(Event, Event.HasField(TEXT("caller")) ? TEXT("caller") : TEXT("waiter"));
        if (Source)
        {
            const FString File = Field(*Source, TEXT("script"));
            if (!File.IsEmpty())
                Detail += FString::Printf(TEXT("  %s:%d  %s"), *File, Source->GetIntegerField(TEXT("line")), *Field(*Source, TEXT("component")));
            else Detail += TEXT("  Native");
        }
        return FString::Printf(TEXT("#%lld   %.3f s   %s\n%s"), static_cast<int64>(Event.GetNumberField(TEXT("sequence"))),
            Event.GetNumberField(TEXT("worldSeconds")), *Type.ToUpper(), *Detail.TrimStartAndEnd());
    }
}

FString SAngelIOMissionInspector::FOperation::Caption() const
{
    return FString::Printf(TEXT("%s  ·  %s  ·  %s  [%s]"), Name.IsEmpty() ? TEXT("Operation") : *Name,
        Target.IsEmpty() ? TEXT("—") : *Target, Status.IsEmpty() ? (bOpen ? TEXT("Pending") : TEXT("No recorded result")) : *Status, *Id.Left(8));
}

void SAngelIOMissionInspector::Construct(const FArguments&)
{
    auto Heading = [](const TCHAR* Text)
    {
        return SNew(STextBlock).Text(FText::FromString(Text)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10));
    };
    ChildSlot
    [
        SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(12)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[Heading(TEXT("PIE world"))]
            + SVerticalBox::Slot().AutoHeight().Padding(0,3,0,8)
            [ SNew(SComboButton).OnGetMenuContent(this, &SAngelIOMissionInspector::WorldMenu)
                .ButtonContent()[SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(WorldName(World.Get())); })] ]
            + SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
            [ SNew(SVerticalBox).Visibility_Lambda([this]{return World.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;})
                + SVerticalBox::Slot().AutoHeight()[Heading(TEXT("World bytecode cache"))]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(BytecodeCacheSummary());}) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(SExpandableArea).InitiallyCollapsed(true).AreaTitle(FText::FromString(TEXT("Scripts and cache activity")))
                    .Visibility_Lambda([this]{return CacheWorld() ? EVisibility::Visible : EVisibility::Collapsed;})
                    .BodyContent()
                    [ SNew(SBox).MaxDesiredHeight(180)
                        [ SNew(SScrollBox) + SScrollBox::Slot()
                            [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return BytecodeCacheDetails();}) ] ] ] ] ]
            + SVerticalBox::Slot().AutoHeight()[Heading(TEXT("Mission"))]
            + SVerticalBox::Slot().AutoHeight().Padding(0,3,0,8)
            [ SNew(SComboButton).IsEnabled_Lambda([this]{return World.IsValid();}).OnGetMenuContent(this, &SAngelIOMissionInspector::MissionMenu)
                .ButtonContent()[SNew(STextBlock).Text_Lambda([this]{ return FText::FromString(MissionName(Mission.Get())); })] ]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Origin);})]
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Open running source")))
                    .IsEnabled_Lambda([this]{return AuthorityMission.IsValid() && !Inspection.SourceRevision.IsEmpty();})
                    .OnClicked_Lambda([this]
                    {
                        FString Path;
                        int32 Line = 1;
                        bool Opened = false;
                        if (AuthorityMission.IsValid() && AuthorityMission->ResolveRunningSource(Path, Line))
                            Opened = FModuleManager::LoadModuleChecked<ISourceCodeAccessModule>("SourceCodeAccess").GetAccessor().OpenFileAtLine(Path, Line);
                        ActionResult = Opened ? FString() : FString::Printf(TEXT("Open %s at line %d in your source editor."), *Path, Line);
                        return FReply::Handled();
                    })]
                + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Validate and restart")))
                    .IsEnabled_Lambda([this]{return AuthorityMission.IsValid() && !Inspection.Script.IsEmpty();})
                    .OnClicked_Lambda([this]
                    {
                        if (AuthorityMission.IsValid()) AuthorityMission->RestartValidated(ActionResult);
                        Refresh();
                        return FReply::Handled();
                    })] ]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true)
                .Text_Lambda([this]{return FText::FromString(ActionResult);})]
            + SVerticalBox::Slot().AutoHeight().Padding(0,8,0,1)
            [ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 13)).Text_Lambda([this]{return FText::FromString(Location);}) ]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Wait);})]
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SVerticalBox).Visibility_Lambda([this]{return Inspection.CheckpointId.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;})
                + SVerticalBox::Slot().AutoHeight().Padding(0,10,0,3)[Heading(TEXT("Mission checkpoint"))]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(CheckpointSummary());}) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(SEditableTextBox).IsReadOnly(true).ToolTipText(FText::FromString(TEXT("Checkpoint ID for read_trace")))
                    .Text_Lambda([this]{return FText::FromString(Inspection.CheckpointId.ToString(EGuidFormats::DigitsWithHyphensLower));}) ] ]
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SVerticalBox).Visibility_Lambda([this]{return Inspection.PersistedIntegers.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;})
                + SVerticalBox::Slot().AutoHeight().Padding(0,10,0,3)[Heading(TEXT("Persisted component state"))]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true)
                    .Text_Lambda([this]{return FText::FromString(PersistedStateSummary());}) ] ]
            + SVerticalBox::Slot().AutoHeight().Padding(0,14,0,3)[Heading(TEXT("Operation history"))]
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(SComboButton).IsEnabled_Lambda([this]{return !Operations.IsEmpty();}).OnGetMenuContent(this, &SAngelIOMissionInspector::OperationMenu)
                .ButtonContent()[SNew(STextBlock).Text_Lambda([this]{return FText::FromString(OperationLabel);})] ]
            + SVerticalBox::Slot().AutoHeight().Padding(0,3,0,4)
            [ SNew(SEditableTextBox).IsReadOnly(true).Visibility_Lambda([this]{return DisplayedId.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;})
                .Text_Lambda([this]{return FText::FromString(DisplayedId);}) ]
            + SVerticalBox::Slot().FillHeight(1)
            [ SNew(SScrollBox) + SScrollBox::Slot()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Timeline);})] ]
        ]
    ];
    Refresh();
    RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
    {
        Refresh();
        return EActiveTimerReturnType::Continue;
    }));
}

TSharedRef<SWidget> SAngelIOMissionInspector::WorldMenu()
{
    FMenuBuilder Menu(true, nullptr);
    for (UWorld* W : PlayWorlds())
        Menu.AddMenuEntry(FText::FromString(WorldName(W)), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateSP(this, &SAngelIOMissionInspector::SelectWorld, TWeakObjectPtr<UWorld>(W))));
    return Menu.MakeWidget();
}

TSharedRef<SWidget> SAngelIOMissionInspector::MissionMenu()
{
    FMenuBuilder Menu(true, nullptr);
    TArray<TPair<FString, TWeakObjectPtr<UAngelIOScriptComponent>>> Choices;
    AActor* Picked = GEditor->GetSelectedActors()->GetTop<AActor>();
    if (Picked && Picked->GetWorld()->WorldType == EWorldType::Editor)
        Picked = EditorUtilities::GetSimWorldCounterpartActor(Picked);
    if (World.IsValid())
        for (TActorIterator<AActor> It(World.Get()); It; ++It)
            for (auto* C : TInlineComponentArray<UAngelIOScriptComponent*>(*It))
                if (IsValid(C) && !C->Script.IsEmpty()) Choices.Emplace(MissionName(C), C);
    Choices.Sort([](const auto& A, const auto& B){return A.Key < B.Key;});
    auto AddChoice = [&](const auto& Choice)
    {
        Menu.AddMenuEntry(FText::FromString(Choice.Key), FText(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateSP(this, &SAngelIOMissionInspector::SelectMission, Choice.Value)));
    };
    if (Picked && Picked->GetWorld() == World.Get())
    {
        Menu.BeginSection(NAME_None, FText::FromString(TEXT("Selected actor")));
        for (const auto& Choice : Choices)
            if (Choice.Value->GetOwner() == Picked) AddChoice(Choice);
        Menu.EndSection();
    }
    Menu.BeginSection(NAME_None, FText::FromString(TEXT("Missions in this world")));
    for (const auto& Choice : Choices)
        if (Choice.Value->GetOwner() != Picked) AddChoice(Choice);
    Menu.EndSection();
    return Menu.MakeWidget();
}

TSharedRef<SWidget> SAngelIOMissionInspector::OperationMenu()
{
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(FText::FromString(TEXT("Follow current or latest operation")), FText(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SAngelIOMissionInspector::SelectOperation, FString())));
    for (const auto& Op : Operations)
        Menu.AddMenuEntry(FText::FromString(Op.Caption()), FText::FromString(Op.Id), FSlateIcon(),
            FUIAction(FExecuteAction::CreateSP(this, &SAngelIOMissionInspector::SelectOperation, Op.Id)));
    return Menu.MakeWidget();
}

void SAngelIOMissionInspector::SelectWorld(TWeakObjectPtr<UWorld> Value)
{
    World = Value;
    SelectMission(nullptr);
}

void SAngelIOMissionInspector::SelectMission(TWeakObjectPtr<UAngelIOScriptComponent> Value)
{
    ActionResult.Reset();
    Mission = Value;
    AuthorityMission.Reset();
    PinnedId.Reset();
    HistoryRevision = -1;
    Refresh();
}

void SAngelIOMissionInspector::SelectOperation(FString Id)
{
    PinnedId = MoveTemp(Id);
    UpdateTimeline();
}

void SAngelIOMissionInspector::Refresh()
{
    const auto Worlds = PlayWorlds();
    if (!Worlds.Contains(World.Get()))
    {
        World.Reset();
        for (UWorld* W : Worlds)
            if (!World.IsValid() || W->GetNetMode() == NM_ListenServer) World = W;
        Mission.Reset();
    }
    auto* Selected = Mission.Get();
    if (!Selected || Selected->GetWorld() != World.Get() || !IsValid(Selected->GetOwner()))
    {
        Mission.Reset();
        AuthorityMission.Reset();
        Inspection = {};
        Operations.Reset();
        PinnedId.Reset();
        HistoryRevision = -1;
        Dropped = 0;
        Origin = World.IsValid() ? TEXT("Choose a mission to inspect.") : TEXT("Start Play In Editor to inspect a mission.");
        Location.Reset();
        Wait.Reset();
        UpdateTimeline();
        return;
    }
    if (!AuthorityMission.IsValid()) AuthorityMission = ResolveAuthority(Selected);
    auto* Authority = AuthorityMission.Get();
    const bool bProxy = Selected->GetNetMode() == NM_Client;
    const FString ReplicaWait = bProxy ? Selected->GetWaitingSignal() : FString();
    if (!Authority)
    {
        Inspection = {};
        Origin = TEXT("Client replica · server VM unavailable in this editor");
        Location = Selected->Script;
        Wait = ReplicaWait.IsEmpty() ? TEXT("No replicated wait") : TEXT("Replicated wait · ") + ReplicaWait;
        Operations.Reset();
        HistoryRevision = -1;
        Dropped = 0;
        UpdateTimeline();
        return;
    }
    Origin = bProxy ? TEXT("Server VM · client replica reports ") + (ReplicaWait.IsEmpty() ? FString(TEXT("no wait")) : ReplicaWait) :
        TEXT("Local authoritative VM");
    const auto Previous = Inspection;
    Inspection = Authority->InspectMission();
    Location = Inspection.Script;
    if (Inspection.Line > 0) Location += FString::Printf(TEXT(":%d"), Inspection.Line);
    switch (Inspection.State)
    {
        case EAngelIOState::Waiting: Wait = TEXT("Waiting for ") + Inspection.Waiting; break;
        case EAngelIOState::Failed: Wait = TEXT("Script failed"); break;
        case EAngelIOState::Finished: Wait = TEXT("Finished"); break;
        case EAngelIOState::Running: Wait = TEXT("Running"); break;
        case EAngelIOState::Compiled: Wait = TEXT("Compiled"); break;
        default: Wait = TEXT("Stopped"); break;
    }
    if (Inspection.Operation.IsValid()) Wait += TEXT("  ·  ") + Inspection.Operation.ToString(EGuidFormats::DigitsWithHyphensLower);
    for (const auto& Branch : Inspection.BranchWaits) Wait += TEXT("\n") + Branch;
    const auto* Trace = Authority->GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>();
    const int64 Revision = Trace ? Trace->GetInspectionRevision() : 0;
    if (HistoryRevision != Revision || Previous.Instance != Inspection.Instance || Previous.Operation != Inspection.Operation)
    {
        if (Previous.Instance != Inspection.Instance) PinnedId.Reset();
        HistoryRevision = Revision;
        CollectHistory(*Authority);
        UpdateTimeline();
    }
}

UWorld* SAngelIOMissionInspector::CacheWorld() const
{
    UWorld* Selected = World.Get();
    if (Selected && Selected->GetNetMode() == NM_Client)
        return AuthorityMission.IsValid() ? AuthorityMission->GetWorld() : nullptr;
    return Selected;
}

FString SAngelIOMissionInspector::BytecodeCacheSummary() const
{
    if (!World.IsValid()) return {};
    UWorld* Source = CacheWorld();
    if (!Source) return TEXT("Client world · select a server world to inspect its cache.");
    const auto Cache = AngelIO::GetBytecodeCacheStats(Source);
    if (!Cache) return TEXT("Waiting for a registered script engine.");
    return FString::Printf(TEXT("%s%sRetained %llu B / budget %llu B  ·  Hits %llu  ·  Evictions %llu"),
        Source != World.Get() ? TEXT("Server world · ") : TEXT(""), Cache->BudgetBytes == 0 ? TEXT("Disabled · ") : TEXT(""),
        Cache->RetainedBytes, Cache->BudgetBytes, Cache->Hits, Cache->Evictions);
}

FText SAngelIOMissionInspector::BytecodeCacheDetails()
{
    UWorld* Source = CacheWorld();
    const auto Stats = AngelIO::GetBytecodeCacheStats(Source);
    if (!Stats) return FText::GetEmpty();
    if (CacheDetailsWorld == Source && CacheDetailsVersion == Stats->InspectionRevision) return CacheDetailsText;
    const auto Cache = AngelIO::GetBytecodeCacheSnapshot(Source);
    if (!Cache) return FText::GetEmpty();
    FString Text = TEXT("Resident scripts (most recently used first)");
    for (const auto& Entry : Cache->ResidentScripts)
        Text.Appendf(TEXT("\n%s  ·  %llu B"), *Entry.Script, Entry.RetainedBytes);
    if (Cache->ResidentScripts.IsEmpty()) Text += TEXT("\nNone");
    Text += TEXT("\n\nRecent cache activity (newest first)");
    for (int32 I = Cache->RecentActivity.Num() - 1; I >= 0; --I)
    {
        const auto& Event = Cache->RecentActivity[I];
        Text.Appendf(TEXT("\n#%llu  %s  ·  %s  ·  %llu B"), Event.Sequence,
            Event.Action == EAngelIOBytecodeCacheAction::Hit ? TEXT("Hit") : TEXT("Evicted"),
            *Event.Entry.Script, Event.Entry.RetainedBytes);
    }
    if (Cache->RecentActivity.IsEmpty()) Text += TEXT("\nNone");
    if (Cache->DroppedActivity) Text.Appendf(TEXT("\n%llu earlier records omitted"), Cache->DroppedActivity);
    CacheDetailsWorld = Source;
    CacheDetailsVersion = Cache->InspectionRevision;
    CacheDetailsText = FText::FromString(MoveTemp(Text));
    return CacheDetailsText;
}

FString SAngelIOMissionInspector::CheckpointSummary() const
{
    if (!Inspection.CheckpointId.IsValid()) return {};
    FString Text = FString::Printf(TEXT("Saved stage: %s (v%d)"), *Inspection.SavedStage, Inspection.SavedVersion);
    if (Inspection.RestoredVersion)
        Text += FString::Printf(TEXT("\nRestored this run: %s (v%d)"), *Inspection.RestoredStage, Inspection.RestoredVersion);
    else
        Text += TEXT("\nNo checkpoint restored this run");
    return Text;
}

FString SAngelIOMissionInspector::PersistedStateSummary() const
{
    TArray<FString> Rows;
    for (const auto& Pair : Inspection.PersistedIntegers)
    {
        const auto& State = Pair.Value;
        const FString Restore = State.Restored.IsSet() ? FString::FromInt(*State.Restored) : TEXT("None this activation");
        Rows.Add(FString::Printf(TEXT("Saved %s: %d\nRestored %s: %s\nState ID: %s"), *State.Field, State.Saved,
            *State.Field, *Restore, *Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower)));
    }
    return FString::Join(Rows, TEXT("\n\n"));
}

void SAngelIOMissionInspector::CollectHistory(UAngelIOScriptComponent& Authority)
{
    Operations.Reset();
    const auto* Trace = Authority.GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>();
    Dropped = Trace ? Trace->GetDroppedEventCount() : 0;
    if (!Trace) return;
    const FString Instance = Inspection.Instance.ToString(EGuidFormats::DigitsWithHyphensLower);
    TMap<FString, FOperation> Related;
    if (Inspection.Operation.IsValid())
        Related.FindOrAdd(Inspection.Operation.ToString(EGuidFormats::DigitsWithHyphensLower)).bOpen = true;
    Trace->VisitExecuting(&Authority, Authority.GetOwner(), [&](const FJsonObject& Call)
    {
        auto& Op = Related.FindOrAdd(Field(Call, TEXT("operationId")));
        Op.Name = Field(Call, TEXT("operation"));
        Op.Target = Field(Call, TEXT("target"));
        Op.bOpen = true;
    });
    Trace->VisitEvents([&](const FJsonObject& Event, const UObject* Target)
    {
        FString OperationId;
        if (!Event.TryGetStringField(TEXT("operationId"), OperationId)) return;
        const auto* Caller = Site(Event, TEXT("caller"));
        const auto* Waiter = Site(Event, TEXT("waiter"));
        if (Target == &Authority || Target == Authority.GetOwner() || Field(Event, TEXT("instanceId")) == Instance ||
            (Caller && Field(*Caller, TEXT("instanceId")) == Instance) ||
            (Waiter && Field(*Waiter, TEXT("instanceId")) == Instance))
            Related.FindOrAdd(OperationId);
    });
    Trace->VisitEvents([&](const FJsonObject& Event, const UObject*)
    {
        auto* Op = Related.Find(Field(Event, TEXT("operationId")));
        if (!Op) return;
        const FString Type = Field(Event, TEXT("type"));
        if (Type == TEXT("decision") || Type.StartsWith(TEXT("signal"))) Op->Status = Field(Event, TEXT("reason"));
        if (Type == TEXT("call") || Type == TEXT("decision") || Type == TEXT("signalSent"))
        {
            Op->Name = Field(Event, TEXT("operation"));
            Op->Target = Field(Event, TEXT("target"));
        }
        else if (Type == TEXT("complete"))
        {
            Op->Status = Field(Event, TEXT("status"));
            Op->bOpen = false;
        }
        else if (Op->Status.IsEmpty() && Field(Event, TEXT("outcome")) != TEXT("TimedOut") && (Type == TEXT("resume") ||
            (Type == TEXT("wait") && !Event.GetBoolField(TEXT("suspended")))))
            Op->Status = TEXT("Completed");
        Op->LastSequence = static_cast<int64>(Event.GetNumberField(TEXT("sequence")));
    });
    for (auto& Pair : Related)
    {
        Pair.Value.Id = Pair.Key;
        Operations.Add(MoveTemp(Pair.Value));
    }
    Operations.Sort([](const auto& A, const auto& B){return A.LastSequence > B.LastSequence;});
}

void SAngelIOMissionInspector::UpdateTimeline()
{
    const FString FollowId = Inspection.Operation.ToString(EGuidFormats::DigitsWithHyphensLower);
    const auto* Op = Operations.FindByPredicate([&](const auto& Item){return Item.Id == PinnedId;});
    if (!Op)
    {
        PinnedId.Reset();
        Op = Operations.FindByPredicate([&](const auto& Item){return Item.Id == FollowId;});
    }
    if (!Op)
    {
        Op = Operations.FindByPredicate([](const auto& Item){return Item.bOpen;});
        if (!Op && !Operations.IsEmpty()) Op = &Operations[0];
    }
    DisplayedId = Op ? Op->Id : FString();
    OperationLabel = Op ? (PinnedId.IsEmpty() ? TEXT("Following   ") : TEXT("Pinned   ")) + Op->Caption() : TEXT("No retained operation");
    TArray<FString> Rows;
    if (Op && AuthorityMission.IsValid())
        if (const auto* Trace = AuthorityMission->GetWorld()->GetSubsystem<UAngelIOTraceSubsystem>())
            Trace->VisitEvents([&](const FJsonObject& Event, const UObject*)
            {
                if (Field(Event, TEXT("operationId")) == DisplayedId) Rows.Add(DescribeEvent(Event));
            });
    Timeline = FString::Join(Rows, TEXT("\n\n"));
    if (Dropped > 0) Timeline = FString::Printf(TEXT("History truncated · %lld older world events discarded\n\n"), Dropped) + Timeline;
}
