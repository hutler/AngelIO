#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AngelIOMapValidation.h"
#include "AngelIOScriptComponent.h"
#include "../AngelIOExampleLift.h"
#include "../AngelIOPlayerController.h"
#include "../AngelIORoomCharacter.h"
#include "../AngelIORoomDoor.h"
#include "../AngelIORoomTerminal.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOMapContract, "AngelIO.Editor.MapContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOMapContract::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    auto* World = Scene.GetTestWorld();
    World->GetWorldSettings()->DefaultGameMode = AAngelIODemoGameMode::StaticClass();
    auto* A = World->SpawnActor<AAngelIOExampleLift>();
    auto* B = World->SpawnActor<AAngelIOExampleLift>();
    if (!A || !B) return false;
    A->Address->SetTargetName(TEXT("FreightLift"));
    B->Address->SetTargetName(TEXT("ServiceLiftB"));
    const auto Healthy = AngelIO::ValidateMap(*World);
    FAngelIOProgram Contract;
    TestTrue(TEXT("Read port defaults without running the mission"), Contract.CompileFile(TEXT("Elevator.as")) &&
        Contract.Ports().ContainsByPredicate([](const auto& P){ return P.Name == TEXT("TravelSpeed") && P.DefaultExpression == TEXT("240.0f"); }));
    TestTrue(TEXT("Mission file selector includes nested scripts"), A->Mission->GetMissionScripts().Contains(TEXT("Examples/RoomDelivery.as")));
    TestEqual(TEXT("Two placed lifts and two controller panels"), Healthy.MissionsChecked, 4);
    TestTrue(TEXT("Group can contain both lifts"), Healthy.Diagnostics.IsEmpty());

    B->Address->SetTargetName(TEXT("FREIGHTLIFT"));
    A->Mission->InitialInputs.Add(TEXT("TravelSpeed"), FAngelIOValue{});
    A->Mission->InitialInputs.Add(TEXT("startingFloor"), FAngelIOValue{});
    B->Mission->Script.Empty();
    auto Broken = AngelIO::ValidateMap(*World);
    TestFalse(TEXT("Invalid contracts fail validation"), Broken.IsValid());
    auto Find = [&](const TCHAR* Code, const TCHAR* Field)
    {
        return Broken.Diagnostics.FindByPredicate([&](const auto& D) { return D.Code == Code && D.Field == Field; });
    };
    TestTrue(TEXT("Unknown case-sensitive input"), Find(TEXT("input_unknown"), TEXT("startingFloor")) != nullptr);
    TestTrue(TEXT("Rejects int input for float"), Find(TEXT("input_type"), TEXT("TravelSpeed")) != nullptr);
    TestTrue(TEXT("Empty auto-start script"), Find(TEXT("script_missing"), TEXT("Script")) != nullptr);
    const auto* Ambiguous = Find(TEXT("target_ambiguous"), TEXT("FreightLift"));
    TestTrue(TEXT("Typed lookup identifies both actors and the panel source line"), Ambiguous && Ambiguous->Line > 0 &&
        Ambiguous->Component->Script == TEXT("TypedLift.as") && Ambiguous->Message.Contains(A->GetPathName()) &&
        Ambiguous->Message.Contains(B->GetPathName()));

    A->Address->SetTargetName(NAME_None);
    B->Address->SetTargetName(NAME_None);
    B->Mission->Script = TEXT("NoSuchAngelIOTestFile.as");
    A->Mission->SetTargetName(NAME_None);
    B->Mission->SetTargetName(NAME_None);
    Broken = AngelIO::ValidateMap(*World);
    TestTrue(TEXT("Missing lookup and missing group"), Find(TEXT("target_missing"), TEXT("FreightLift")) &&
        Find(TEXT("target_missing"), TEXT("DemoLifts")));
    TestTrue(TEXT("File error identifies the owning component"),
        Find(TEXT("script_compile"), TEXT("Script")) && Find(TEXT("script_compile"), TEXT("Script"))->Component == B->Mission);

    FAngelIOProgram Calls;
    const TCHAR* Source = TEXT(R"(namespace Helper {
        void FindLift(const string &in name) {}
        void Local() { FindLift("NotAnEndpoint"); }
    }
    void Run() {
        // SendTo("Comment", "", 0);
        string name = "Dynamic";
        SendTo(name, "Go", 1);
        SendTo("Prefix" + name, "Go", 1);
        SendTo(
            "LiteralGroup", "Go", 1);
    })");
    if (!TestTrue(TEXT("Compile direct, computed, commented and shadowed calls"), Calls.Compile(TEXT("Calls.as"), Source))) return false;
    const auto& Literals = Calls.LiteralTargets();
    TestTrue(TEXT("Only the direct group call is checked, with its original line"), Literals.Num() == 1 &&
        Literals[0].Name == TEXT("LiteralGroup") && Literals[0].Line == 10);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIODoorRoomContract, "AngelIO.Editor.DoorRoomContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIODoorRoomContract::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game)) return false;
    auto* World = Scene.GetTestWorld();
    World->GetWorldSettings()->DefaultGameMode = AAngelIORoomGameMode::StaticClass();
    auto* Terminal = World->SpawnActor<AAngelIORoomTerminal>();
    if (!Terminal) return false;
    const auto Missing = AngelIO::ValidateMap(*World);
    const auto* MissingDoor = Missing.Diagnostics.FindByPredicate([&](const auto& D)
        { return D.Component == Terminal->Mission && D.Field == TEXT("Door") && D.Code == TEXT("target_missing"); });
    TestTrue(TEXT("A missing room door reports the lookup line"), MissingDoor && MissingDoor->Line > 0);
    Terminal->Mission->SetTargetName(TEXT("RoomDoor"));
    const auto Wrong = AngelIO::ValidateMap(*World);
    TestTrue(TEXT("Wrong type has its own diagnostic, with the actor and expected class"), Wrong.Diagnostics.ContainsByPredicate(
        [&](const auto& D) { return D.Code == TEXT("target_wrong_class") && D.Line > 0 &&
            D.Message.Contains(Terminal->GetPathName()) && D.Message.Contains(AAngelIORoomDoor::StaticClass()->GetPathName()); }));
    auto* First = World->SpawnActor<AAngelIORoomDoor>();
    if (!First) return false;
    const auto Healthy = AngelIO::ValidateMap(*World);
    TestTrue(TEXT("One matching door plus a different actor class satisfies the local contract"), Healthy.IsValid() &&
        Healthy.Roles.ContainsByPredicate([](const auto& R) { return R.Role == TEXT("Door") && R.bRoom &&
            R.bSingle && !R.bMayStream && R.Matches.Num() == 1 && R.WrongClass.Num() == 1; }));
    auto* Second = World->SpawnActor<AAngelIORoomDoor>(FVector(400, 0, 0), FRotator::ZeroRotator);
    if (!Second) return false;
    const auto Ambiguous = AngelIO::ValidateMap(*World);
    const auto* Duplicate = Ambiguous.Diagnostics.FindByPredicate([&](const auto& D)
        { return D.Component == Terminal->Mission && D.Field == TEXT("Door") && D.Code == TEXT("target_ambiguous"); });
    TestTrue(TEXT("Two matching local doors identify both actors and the source line"), Duplicate &&
        Duplicate->Line > 0 && Duplicate->Message.Contains(First->GetPathName()) && Duplicate->Message.Contains(Second->GetPathName()));
    return true;
}
#endif
