#include "AngelIOCheckpointSubsystem.h"
#include "Algo/AllOf.h"
#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "Misc/Paths.h"

namespace
{
    bool BindMission(FSQLitePreparedStatement& Statement, const UActorComponent& Component, FString Script)
    {
        const ULevel* Level = Component.GetOwner()->GetLevel();
        const UWorld* World = Component.GetWorld();
        const ULevelStreaming* Stream = FLevelUtils::FindStreamingLevel(Level);
        auto Package = [World](const FString& Name)
        {
            return World->StreamingLevelsPrefix.IsEmpty() ? Name :
                UWorld::StripPIEPrefixFromPackageName(Name, World->StreamingLevelsPrefix);
        };
        const FString Room = Package(Stream ? Stream->GetWorldAssetPackageName() : Level->GetOutermost()->GetName());
        const FString Owner = Room + TEXT(":") + Component.GetPathName(Level);
        FPaths::NormalizeFilename(Script);
        FPaths::CollapseRelativeDirectories(Script);
        return Statement.IsValid() &&
            Statement.SetBindingValueByIndex(1, Package(World->GetOutermost()->GetName()).ToLower()) &&
            Statement.SetBindingValueByIndex(2, Owner.ToLower()) && Statement.SetBindingValueByIndex(3, Script.ToLower());
    }
}

bool UAngelIOCheckpointSubsystem::Open(FString& Error, bool bInspectOnly)
{
    if (Database.IsValid()) return true;
    const UWorld* World = GetWorld();
    const FString Slot = World->URL.GetOption(TEXT("AngelIOCheckpointSlot="), TEXT("Default"));
    if (Slot.Len() > 64 || !Algo::AllOf(Slot, [](TCHAR C)
        { return FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-'); }))
    {
        Error = TEXT("Invalid AngelIOCheckpointSlot (use up to 64 letters, digits, underscores or hyphens)");
        return false;
    }
    const FString Path = Slot.IsEmpty() ? FString(TEXT(":memory:")) :
        FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints") /
            (World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Game")) / (TEXT("slot-") + Slot + TEXT(".sqlite3")));
    if (bInspectOnly && (Slot.IsEmpty() || !FPaths::FileExists(Path))) return true;
    if (!Database.Open(*Path))
    {
        Error = TEXT("Could not open checkpoint store: ") + Path;
        return false;
    }
    if (!Database.Execute(TEXT("PRAGMA synchronous=FULL")) ||
        !Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS checkpoints (")
            TEXT("map TEXT NOT NULL, owner TEXT NOT NULL, script TEXT NOT NULL, id BLOB NOT NULL, ")
            TEXT("writer BLOB NOT NULL, version INTEGER NOT NULL CHECK(version > 0), stage BLOB NOT NULL, ")
            TEXT("PRIMARY KEY(map, owner, script)) WITHOUT ROWID")))
    {
        Error = TEXT("Could not initialize checkpoint store: ") + Database.GetLastError();
        Database.Close();
        return false;
    }
    return true;
}

bool UAngelIOCheckpointSubsystem::Load(const UActorComponent& Component, const FString& Script,
    FCheckpoint& Out, FString& Error, bool bInspectOnly)
{
    Out = {};
    if (!Open(Error, bInspectOnly)) return false;
    if (!Database.IsValid()) return true;
    auto Query = Database.PrepareStatement(TEXT("SELECT id, writer, version, stage FROM checkpoints WHERE map=?1 AND owner=?2 AND script=?3"));
    if (BindMission(Query, Component, Script))
    {
        const auto Result = Query.Step();
        if (Result == ESQLitePreparedStatementStepResult::Done) return true;
        if (Result == ESQLitePreparedStatementStepResult::Row)
        {
            TArray<uint8> Stage;
            if (Query.GetColumnValueByIndex(0, Out.Id) && Out.Id.IsValid() &&
                Query.GetColumnValueByIndex(1, Out.SavedBy) && Out.SavedBy.IsValid() &&
                Query.GetColumnValueByIndex(2, Out.Version) && Out.Version > 0 &&
                Query.GetColumnValueByIndex(3, Stage) && !Stage.IsEmpty() && !Stage.Contains(0))
            {
                Out.Stage = FUtf8String(Stage.Num(), reinterpret_cast<const UTF8CHAR*>(Stage.GetData()));
                return true;
            }
            Error = TEXT("Invalid mission checkpoint record");
            return false;
        }
    }
    Error = TEXT("Could not read mission checkpoint: ") + Database.GetLastError();
    return false;
}

bool UAngelIOCheckpointSubsystem::Save(const UActorComponent& Component, const FString& Script,
    FCheckpoint& Record, FString& Error)
{
    if (!Open(Error, false)) return false;
    // One SQLite transaction. Keep the existing ID across writers, version changes and restarts.
    auto Write = Database.PrepareStatement(TEXT("INSERT INTO checkpoints(map,owner,script,id,writer,version,stage) ")
        TEXT("VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(map,owner,script) DO UPDATE SET ")
        TEXT("writer=excluded.writer, version=excluded.version, stage=excluded.stage RETURNING id"));
    if (BindMission(Write, Component, Script) && Write.SetBindingValueByIndex(4, FGuid::NewGuid()) &&
        Write.SetBindingValueByIndex(5, Record.SavedBy) && Write.SetBindingValueByIndex(6, Record.Version) &&
        Write.SetBindingValueByIndex(7, *Record.Stage, Record.Stage.Len()) &&
        Write.Step() == ESQLitePreparedStatementStepResult::Row && Write.GetColumnValueByIndex(0, Record.Id) &&
        Write.Step() == ESQLitePreparedStatementStepResult::Done)
        return true;
    Error = TEXT("Could not commit mission checkpoint: ") + Database.GetLastError();
    return false;
}

void UAngelIOCheckpointSubsystem::Deinitialize()
{
    Database.Close();
    Super::Deinitialize();
}
