#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAngelIOTrace, Log, All);

bool UAngelIOTraceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const UWorld* World = Cast<UWorld>(Outer);
    return !UE_BUILD_SHIPPING && Super::ShouldCreateSubsystem(Outer) && World &&
        (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

TSharedRef<FJsonObject> UAngelIOTraceSubsystem::Append(const FGuid& OperationId, const TCHAR* EventType, const UObject* Target)
{
    check(IsInGameThread());
    if (!SessionId.IsValid())
    {
        SessionId = FGuid::NewGuid();
        const ENetMode Mode = GetWorld()->GetNetMode();
        NetMode = Mode == NM_ListenServer ? TEXT("listenServer") :
            Mode == NM_DedicatedServer ? TEXT("dedicatedServer") : TEXT("standalone");
    }
    auto Event = MakeShared<FJsonObject>();
    Event->SetNumberField(TEXT("sequence"), Total + 1);
    Event->SetNumberField(TEXT("worldSeconds"), GetWorld()->GetTimeSeconds());
    if (OperationId.IsValid())
        Event->SetStringField(TEXT("operationId"), OperationId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Event->SetStringField(TEXT("type"), EventType);
    FEntry Entry;
    Entry.Json = Event;
#if WITH_EDITOR
    Entry.Target = Target;
    if (Target && FCString::Strcmp(EventType, TEXT("call")) == 0) LiveTargets.Add(OperationId, Entry);
    if (FCString::Strcmp(EventType, TEXT("complete")) == 0) LiveTargets.Remove(OperationId);
#endif
    if (Events.Num() < Capacity) Events.Add(MoveTemp(Entry));
    else Events[Total % Capacity] = MoveTemp(Entry);
    ++Total;
    return Event;
}

void UAngelIOTraceSubsystem::RecordCheckpoint(bool bSaved, const FGuid& CheckpointId, const FString& Stage, int32 Version,
    const FGuid& SavedBy, TSharedRef<FJsonObject> Caller)
{
    auto Event = Append({}, bSaved ? TEXT("checkpointSaved") : TEXT("checkpointRestored"), nullptr);
    Event->SetStringField(TEXT("checkpointId"), CheckpointId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Event->SetStringField(TEXT("stage"), Stage);
    Event->SetNumberField(TEXT("version"), Version);
    Event->SetStringField(TEXT("savedByInstanceId"), SavedBy.ToString(EGuidFormats::DigitsWithHyphensLower));
    Event->SetObjectField(TEXT("caller"), Caller);
}

bool UAngelIOTraceSubsystem::Export(FString& OutFile) const
{
    check(IsInGameThread());
    OutFile.Reset();
    if (Events.IsEmpty()) return false;
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), 2);
    Root->SetStringField(TEXT("sessionId"), SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Root->SetStringField(TEXT("world"), GetWorld()->GetName());
    Root->SetStringField(TEXT("netMode"), NetMode);
    Root->SetNumberField(TEXT("droppedEvents"), Total - Events.Num());
    TArray<TSharedPtr<FJsonValue>> Ordered;
    Ordered.Reserve(Events.Num());
    for (int64 I = Total - Events.Num(); I < Total; ++I)
        Ordered.Add(MakeShared<FJsonValueObject>(Events[I % Capacity].Json));
    Root->SetArrayField(TEXT("events"), MoveTemp(Ordered));

    FString Json;
    if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json))) return false;
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AngelIO/Traces"));
    auto& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*Directory, true)) return false;
    // Publish each snapshot only after the entire document has been written.
    const FString Filename = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S-%s")) + TEXT("-") +
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json");
    const FString Path = Directory / Filename;
    const FString Temp = Path + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !Files.Move(*Path, *Temp, false))
    {
        Files.Delete(*Temp);
        return false;
    }
    OutFile = Path;
    UE_LOG(LogAngelIOTrace, Display, TEXT("Exported %d events to %s"), Events.Num(), *Path);
    return true;
}

void UAngelIOTraceSubsystem::Deinitialize()
{
    if (GetWorld()->WorldType == EWorldType::PIE && !Events.IsEmpty())
    {
        FString Path;
        if (!Export(Path)) UE_LOG(LogAngelIOTrace, Warning, TEXT("Could not export PIE operation trace"));
    }
    Super::Deinitialize();
}

#if WITH_EDITOR
void UAngelIOTraceSubsystem::VisitExecuting(const UObject* Component, const UObject* Actor,
    TFunctionRef<void(const FJsonObject&)> Visitor) const
{
    check(IsInGameThread());
    for (const auto& Pair : LiveTargets)
        if (const auto* Target = Pair.Value.Target.Get(); Target && (Target == Component || Target == Actor))
            Visitor(*Pair.Value.Json);
}

void UAngelIOTraceSubsystem::VisitEvents(TFunctionRef<void(const FJsonObject&, const UObject*)> Visitor) const
{
    check(IsInGameThread());
    for (int64 I = Total - Events.Num(); I < Total; ++I)
    {
        const auto& Entry = Events[I % Capacity];
        Visitor(*Entry.Json, Entry.Target.Get());
    }
}
#endif

static FAutoConsoleCommandWithWorld ExportTraceCommand(TEXT("AngelIO.ExportTrace"),
    TEXT("Export this world's mission history to Saved/AngelIO/Traces."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
    {
        auto* Trace = World ? World->GetSubsystem<UAngelIOTraceSubsystem>() : nullptr;
        FString File;
        if (!Trace || !Trace->Export(File))
            UE_LOG(LogAngelIOTrace, Warning, TEXT("No operation trace exported for this world"));
    }));
