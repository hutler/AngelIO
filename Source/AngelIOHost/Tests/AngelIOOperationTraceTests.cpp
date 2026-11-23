#if WITH_DEV_AUTOMATION_TESTS
#include "AngelIOProgram.h"
#include "AngelIOScriptAPI.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"

THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    struct FSharedAction : FAngelIOOperation
    {
        explicit FSharedAction(UWorld* World = nullptr) : FAngelIOOperation(World, TEXT("Test.Action")) {}
        void Finish() { MarkComplete(); }
    };

    // Simulate a module that exposes the same native action to independent VMs.
    struct FSharedActionAPI final : IAngelIOScriptAPI
    {
        TSharedPtr<FSharedAction> Action;
        static void Begin(asIScriptGeneric* Call)
        {
            static_cast<FSharedActionAPI*>(Call->GetAuxiliary())->Action = MakeShared<FSharedAction>();
        }
        static void Wait(asIScriptGeneric* Call)
        {
            auto& API = *static_cast<FSharedActionAPI*>(Call->GetAuxiliary());
            AngelIO::AwaitOperation(API.Action.ToSharedRef(), TEXT("SharedAction"));
        }
        bool RegisterBindings(asIScriptEngine& Engine) override
        {
            return Engine.RegisterGlobalFunction("void StartShared()", asFUNCTION(Begin), asCALL_GENERIC, this) >= 0 &&
                Engine.RegisterGlobalFunction("void WaitShared()", asFUNCTION(Wait), asCALL_GENERIC, this) >= 0;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOSharedTrace, "AngelIO.Trace.IndependentWaiters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOSharedTrace::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    UWorld* World = Scene.GetTestWorld();
    FSharedActionAPI API;
    auto& Features = IModularFeatures::Get();
    Features.RegisterModularFeature(API.FeatureName(), &API);
    ON_SCOPE_EXIT { Features.UnregisterModularFeature(API.FeatureName(), &API); };
    FAngelIOProgram Producer, Consumer;
    if (!TestTrue(TEXT("Producer creates shared action"), Producer.Compile(TEXT("producer.as"),
        TEXT("void Run() { StartShared(); }")) && Producer.Start({}, World) && API.Action)) return false;
    {
        FAngelIOProgram Stopped;
        if (!TestTrue(TEXT("Second script starts waiting"), Stopped.Compile(TEXT("stopped.as"),
            TEXT("void Run() { WaitShared(); }")) && Stopped.Start({}, World) && Stopped.IsWaitingOnOperation())) return false;
    }
    if (!TestTrue(TEXT("Third script starts waiting"), Consumer.Compile(TEXT("consumer.as"),
        TEXT("void Run() { WaitShared(); }")) && Consumer.Start({}, World) && Consumer.IsWaitingOnOperation())) return false;
    API.Action->Finish();
    API.Action->Finish();
    TestTrue(TEXT("Remaining waiter resumes"), Consumer.ResumeCompletedOperation());
    FString Path, Text;
    if (!TestTrue(TEXT("Export shared action"), World->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path))) return false;
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };
    TSharedPtr<FJsonObject> Trace;
    if (!TestTrue(TEXT("Read exported trace"), FFileHelper::LoadFileToString(Text, *Path) &&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Trace))) return false;
    TArray<FString> Timeline;
    for (const auto& Entry : Trace->GetArrayField(TEXT("events")))
    {
        const auto Event = Entry->AsObject();
        TestEqual(TEXT("Common ID"), Event->GetStringField(TEXT("operationId")),
            API.Action->GetOperationId().ToString(EGuidFormats::DigitsWithHyphensLower));
        FString Type = Event->GetStringField(TEXT("type"));
        if (Type != TEXT("complete"))
        {
            const auto Site = Event->GetObjectField(Type == TEXT("call") ? TEXT("caller") : TEXT("waiter"));
            Type += TEXT("/") + Site->GetStringField(TEXT("script"));
        }
        Timeline.Add(Type);
    }
    TestEqual(TEXT("Independent waiters, one completion"), FString::Join(Timeline, TEXT(" ")),
        FString(TEXT("call/producer.as wait/stopped.as detach/stopped.as wait/consumer.as complete resume/consumer.as")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOTraceRetention, "AngelIO.Trace.Retention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOTraceRetention::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    UWorld* World = Scene.GetTestWorld();
    for (int32 I = 0; I < 2050; ++I)
    {
        FSharedAction Action(World);
        Action.Finish();
    }
    FString Path, Text;
    if (!TestTrue(TEXT("Export wrapped buffer"), World->GetSubsystem<UAngelIOTraceSubsystem>()->Export(Path))) return false;
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };
    TSharedPtr<FJsonObject> Trace;
    if (!TestTrue(TEXT("Read bounded history"), FFileHelper::LoadFileToString(Text, *Path) &&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Trace))) return false;
    TestEqual(TEXT("Discarded prefix"), Trace->GetIntegerField(TEXT("droppedEvents")), 4);
    const auto& Events = Trace->GetArrayField(TEXT("events"));
    TestEqual(TEXT("Bounded event count"), Events.Num(), 4096);
    for (int32 I = 0; I < Events.Num(); ++I)
        if (!TestEqual(TEXT("Chronological after wrap"), Events[I]->AsObject()->GetIntegerField(TEXT("sequence")), I + 5))
            return false;
    return true;
}
#endif
