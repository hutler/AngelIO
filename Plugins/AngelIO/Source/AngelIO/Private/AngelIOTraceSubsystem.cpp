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

TSharedRef<FJsonObject> UAngelIOTraceSubsystem::Append(const FGuid& OperationId, const TCHAR* EventType)
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
    Event->SetStringField(TEXT("operationId"), OperationId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Event->SetStringField(TEXT("type"), EventType);
    if (Events.Num() < Capacity) Events.Add(Event);
    else Events[Total % Capacity] = Event;
    ++Total;
    return Event;
}

bool UAngelIOTraceSubsystem::Export(FString& OutFile) const
{
    check(IsInGameThread());
    OutFile.Reset();
    if (Events.IsEmpty()) return false;
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), 1);
    Root->SetStringField(TEXT("sessionId"), SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Root->SetStringField(TEXT("world"), GetWorld()->GetName());
    Root->SetStringField(TEXT("netMode"), NetMode);
    Root->SetNumberField(TEXT("droppedEvents"), Total - Events.Num());
    TArray<TSharedPtr<FJsonValue>> Ordered;
    Ordered.Reserve(Events.Num());
    for (int64 I = Total - Events.Num(); I < Total; ++I)
        Ordered.Add(MakeShared<FJsonValueObject>(Events[I % Capacity]));
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

static FAutoConsoleCommandWithWorld ExportTraceCommand(TEXT("AngelIO.ExportTrace"),
    TEXT("Export this world's operation history to Saved/AngelIO/Traces."),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
    {
        auto* Trace = World ? World->GetSubsystem<UAngelIOTraceSubsystem>() : nullptr;
        FString File;
        if (!Trace || !Trace->Export(File))
            UE_LOG(LogAngelIOTrace, Warning, TEXT("No operation trace exported for this world"));
    }));
