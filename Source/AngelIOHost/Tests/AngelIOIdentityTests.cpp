#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOPersistenceIdentity.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOMapValidation.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOIdentityTest, "AngelIO.Editor.PersistenceIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAngelIOIdentityTest::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    auto* World = Scene.GetTestWorld();
    auto Place = [&]()
    {
        auto* A = World->SpawnActor<AActor>();
        auto* C = NewObject<UAngelIOScriptComponent>(A, TEXT("Logic"));
        A->AddInstanceComponent(C);
        C->bStartOnBeginPlay = false;
        C->RegisterComponent();
        return C;
    };
    auto* First = Place();
    const auto Legacy = FAngelIOPersistenceIdentity::From(*First);
    TestEqual(TEXT("Unversioned components retain their path address"), Legacy.Owner, Legacy.LegacyOwner);
    First->AssignPersistenceId();
    First->PersistenceContract = TEXT("Delivery");
    const auto Before = FAngelIOPersistenceIdentity::From(*First);
    const FString OldPath = First->GetPathName(First->GetOwner()->GetLevel());
    First->FormerPlacementPaths.Add(OldPath);
    First->GetOwner()->Rename(TEXT("RenamedDeliveryOwner"));
    const auto Renamed = FAngelIOPersistenceIdentity::From(*First);
    TestTrue(TEXT("The placement and contract survive object and script rename"), Before.Owner == Renamed.Owner &&
        Renamed.LegacyOwners.Contains(Before.LegacyOwner) &&
        Before.MissionKey(*First, TEXT("old.as")) == Renamed.MissionKey(*First, TEXT("new.as")));
    auto* Copy = Place();
    Copy->PersistenceId = First->PersistenceId;
    auto HasError = [&](const TCHAR* Code)
    {
        return AngelIO::ValidateMap(*World).Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == Code; });
    };
    TestTrue(TEXT("Matching IDs fail preflight"), HasError(TEXT("persistence_id_collision")));
    Copy->PostEditImport();
    TestTrue(TEXT("Editor copy has a new ID"), Copy->PersistenceId.IsValid() && Copy->PersistenceId != First->PersistenceId &&
        !HasError(TEXT("persistence_id_collision")));
    Copy->FormerPlacementPaths.Add(OldPath);
    TestTrue(TEXT("Competing aliases fail preflight"), HasError(TEXT("persistence_alias_collision")));
    return true;
}
#endif
