#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOCheckpointSubsystem.h"
#include "AngelIOPersistenceIdentity.h"
#include "AngelIOScriptComponent.h"
#include "Tests/AutomationCommon.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOLegacySlot, "AngelIO.Persistence.LegacyMigrationAndReceipts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIOLegacySlot::RunTest(const FString&)
{
    const FString Slot = TEXT("Legacy-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints/Game") /
        (TEXT("slot-") + Slot + TEXT(".sqlite3")));
    const FString Backup = Path + TEXT(".pre-v1");
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); IFileManager::Get().Delete(*Backup); };
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    auto* World = Scene.GetTestWorld();
    World->URL.AddOption(*(TEXT("AngelIOCheckpointSlot=") + Slot));
    auto* Owner = World->SpawnActor<AActor>();
    auto* M = NewObject<UAngelIOScriptComponent>(Owner);
    Owner->AddInstanceComponent(M);
    M->bStartOnBeginPlay = false;
    M->RegisterComponent();
    M->AssignPersistenceId();
    M->PersistenceContract = TEXT("Delivery");
    M->FormerPlacementPaths.Add(TEXT("OldTerminal.Mission"));
    M->FormerScripts.Add(TEXT("OldDelivery.as"));
    const auto Identity = FAngelIOPersistenceIdentity::From(*M);
    auto* Store = World->GetSubsystem<UAngelIOCheckpointSubsystem>();
    const FGuid Original = FGuid::NewGuid();
    {
        FSQLiteDatabase Db;
        ON_SCOPE_EXIT { Db.Close(); };
        if (!TestTrue(TEXT("Open legacy fixture"), Db.Open(*Path)) || !TestTrue(TEXT("Create legacy table"), Db.Execute(TEXT("CREATE TABLE checkpoints(map TEXT,owner TEXT,script TEXT,id BLOB,writer BLOB,version INTEGER,stage BLOB,PRIMARY KEY(map,owner,script)) WITHOUT ROWID")))) return false;
        auto Insert = Db.PrepareStatement(TEXT("INSERT INTO checkpoints VALUES(?1,?2,'olddelivery.as',?3,?4,1,X'436F6E6669726D696E67')"));
        if (!Insert.SetBindingValueByIndex(1, Identity.LegacyMap) ||
            !Insert.SetBindingValueByIndex(2, Identity.LegacyOwners.Last()) ||
            !Insert.SetBindingValueByIndex(3, Original) || !Insert.SetBindingValueByIndex(4, FGuid::NewGuid()) || !Insert.Execute())
        { AddError(TEXT("Seeding original checkpoint: ") + Db.GetLastError()); return false; }
    }
    const FString Script = TEXT("Examples/TerminalDelivery.as");
    UAngelIOCheckpointSubsystem::FCheckpoint Record;
    FString Error;
    TestTrue(TEXT("Read-only inspection resolves the old path without migration or backup"), Store->Load(*M, Script, Record, Error, true) &&
        Record.Id == Original && !FPaths::FileExists(Backup));
    if (!TestTrue(TEXT("First writer backs up and adopts the old checkpoint in place"), Store->Load(*M, Script, Record, Error) &&
        Record.Id == Original && FString(Record.Stage) == TEXT("Confirming") && FPaths::FileExists(Backup))) return false;
    FGuid First, Replay;
    TestTrue(TEXT("The grant commits before the final mission checkpoint"), Store->RecordOnce(*M, Script, TEXT("DeliveryCredit"), First, Error));
    FAngelIOProgram Recovered;
    FAngelIOValue Credits;
    if (!TestTrue(TEXT("Real delivery recovers Confirming after the grant and publishes one credit"), Recovered.CompileFile(Script, World) &&
        Recovered.Start({}, World, M) && Recovered.GetOutput(TEXT("Credits"), Credits) && Credits.Integer == 1)) return false;
    TestTrue(TEXT("Replaying the grant preserves its original identity"), Store->RecordOnce(*M, Script, TEXT("DeliveryCredit"), Replay, Error) &&
        First.IsValid() && First == Replay);
    TestTrue(TEXT("Recovery advanced the same checkpoint"), Store->Load(*M, Script, Record, Error) &&
        Record.Id == Original && FString(Record.Stage) == TEXT("Complete"));
    auto Scalar = [](FSQLiteDatabase& Db, const TCHAR* SQL)
    {
        int32 Value = -1;
        auto Q = Db.PrepareStatement(SQL);
        if (Q.Step() == ESQLitePreparedStatementStepResult::Row) Q.GetColumnValueByIndex(0, Value);
        return Value;
    };
    FSQLiteDatabase Snapshot, Live;
    ON_SCOPE_EXIT { Snapshot.Close(); Live.Close(); };
    Store->CloseSlot();
    if (!TestTrue(TEXT("Open backup for inspection"), Snapshot.Open(*Backup, ESQLiteDatabaseOpenMode::ReadOnly)) ||
        !TestTrue(TEXT("Open live slot for fault injection"), Live.Open(*Path))) return false;
    TestEqual(TEXT("Backup stays in its original schema"), Scalar(Snapshot, TEXT("PRAGMA user_version")), 0);
    TestEqual(TEXT("Backup contains the unmodified legacy row"), Scalar(Snapshot, TEXT("SELECT count(*) FROM checkpoints WHERE script='olddelivery.as'")), 1);
    TestEqual(TEXT("One durable credit"), Scalar(Live, TEXT("SELECT count(*) FROM action_receipts")), 1);
    TestEqual(TEXT("One stable checkpoint across migration and replay"), Scalar(Live, TEXT("SELECT count(*) FROM checkpoints")), 1);
    if (!Live.Execute(TEXT("UPDATE checkpoints SET version=2"))) { AddError(Live.GetLastError()); return false; }
    Live.Close();
    FAngelIOProgram Strict;
    TestTrue(TEXT("An incompatible stage cannot accidentally start the delivery fresh"), Strict.CompileFile(Script, World) &&
        !Strict.Start({}, World, M) && Strict.Diagnostics().ContainsByPredicate([](const auto& D)
            { return D.Message.Contains(TEXT("Incompatible mission checkpoint")); }));
    Store->CloseSlot();
    if (!Live.Open(*Path) || !Live.Execute(TEXT("UPDATE checkpoints SET stage=X'00'"))) { AddError(Live.GetLastError()); return false; }
    Live.Close();
    TestTrue(TEXT("Malformed data is a storage error"), !Store->Load(*M, Script, Record, Error) && Error.Contains(TEXT("Invalid mission checkpoint")));
    return true;
}
#endif
