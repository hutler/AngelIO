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
        explicit FSharedAction(UWorld* World = nullptr, const UObject* Target = nullptr) :
            FAngelIOOperation(World, TEXT("Test.Action"), {}, nullptr, Target) {}
        void Finish() { MarkComplete(); }
    };

    // Simulate a module that exposes the same native action to independent VMs.
    struct FSharedActionAPI final : IAngelIOScriptAPI
    {
        TSharedPtr<FSharedAction> Action;
        TSharedPtr<const EAngelIOWaitOutcome> TimedWait;
        static void Begin(asIScriptGeneric* Call)
        {
            static_cast<FSharedActionAPI*>(Call->GetAuxiliary())->Action = MakeShared<FSharedAction>();
        }
        static void Wait(asIScriptGeneric* Call)
        {
            auto& API = *static_cast<FSharedActionAPI*>(Call->GetAuxiliary());
            if (Call->GetArgCount())
                API.TimedWait = AngelIO::AwaitOperation(API.Action.ToSharedRef(), TEXT("SharedAction"), Call->GetArgFloat(0));
            else
                AngelIO::AwaitOperation(API.Action.ToSharedRef(), TEXT("SharedAction"));
        }
        bool RegisterBindings(asIScriptEngine& Engine) override
        {
            return Engine.RegisterGlobalFunction("void StartShared()", asFUNCTION(Begin), asCALL_GENERIC, this) >= 0 &&
                Engine.RegisterGlobalFunction("void WaitShared()", asFUNCTION(Wait), asCALL_GENERIC, this) >= 0 &&
                Engine.RegisterGlobalFunction("void WaitShared(float)", asFUNCTION(Wait), asCALL_GENERIC, this) >= 0;
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
    FAngelIOProgram Timed;
    if (!TestTrue(TEXT("Fourth VM waits for the same operation"), Timed.Compile(TEXT("timed.as"),
        TEXT("void Run() { WaitShared(0.02f); }")) && Timed.Start({}, World) && Timed.IsWaitingOnOperation())) return false;
    TestFalse(TEXT("Deadline is in the future"), Timed.ResumeCompletedOperation());
    Scene.TickTestWorld(0.05f);
    TestTrue(TEXT("Only the timed VM resumes"), Timed.ResumeCompletedOperation() &&
        Timed.State() == EAngelIOState::Finished && *API.TimedWait == EAngelIOWaitOutcome::TimedOut);
    TestFalse(TEXT("Timeout leaves shared operation pending"), API.Action->IsComplete());
    TestFalse(TEXT("Other VM keeps waiting"), Consumer.ResumeCompletedOperation());
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
        if (Type == TEXT("resume") && Event->GetObjectField(TEXT("waiter"))->GetStringField(TEXT("script")) == TEXT("timed.as"))
            TestEqual(TEXT("Resume records waiter outcome"), Event->GetStringField(TEXT("outcome")), FString(TEXT("TimedOut")));
        if (Type != TEXT("complete"))
        {
            const auto Site = Event->GetObjectField(Type == TEXT("call") ? TEXT("caller") : TEXT("waiter"));
            Type += TEXT("/") + Site->GetStringField(TEXT("script"));
        }
        Timeline.Add(Type);
    }
    TestEqual(TEXT("Independent waiters, one completion"), FString::Join(Timeline, TEXT(" ")),
        FString(TEXT("call/producer.as wait/stopped.as detach/stopped.as wait/consumer.as wait/timed.as resume/timed.as complete resume/consumer.as")));
    FAngelIOProgram Tie;
    if (!TestTrue(TEXT("Start a fresh finite wait"), Tie.Compile(TEXT("tie.as"),
        TEXT("void Run() { StartShared(); WaitShared(0.02f); }")) && Tie.Start({}, World))) return false;
    Scene.TickTestWorld(0.05f);
    API.Action->Finish();
    TestTrue(TEXT("Completion wins if observed on the same update as the expired deadline"),
        Tie.ResumeCompletedOperation() && *API.TimedWait == EAngelIOWaitOutcome::Completed);
    FAngelIOProgram Invalid;
    TestTrue(TEXT("Negative timeout reports script error"), Invalid.Compile(TEXT("invalid.as"),
        TEXT("void Run() { WaitShared(-1); }")) && !Invalid.Start({}, World) &&
        Invalid.Diagnostics().ContainsByPredicate([](const auto& D) { return D.Message.Contains(TEXT("non-negative")); }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelIOTraceRetention, "AngelIO.Trace.Retention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAngelIOTraceRetention::RunTest(const FString&)
{
    FTestWorldWrapper Scene;
    if (!Scene.CreateTestWorld(EWorldType::Game) || !Scene.BeginPlayInTestWorld()) return false;
    UWorld* World = Scene.GetTestWorld();
    FSharedAction LongRide(World, World);
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
    TestEqual(TEXT("Discarded prefix"), Trace->GetIntegerField(TEXT("droppedEvents")), 5);
    const auto& Events = Trace->GetArrayField(TEXT("events"));
    TestEqual(TEXT("Bounded event count"), Events.Num(), 4096);
    for (int32 I = 0; I < Events.Num(); ++I)
        if (!TestEqual(TEXT("Chronological after wrap"), Events[I]->AsObject()->GetIntegerField(TEXT("sequence")), I + 6))
            return false;
#if WITH_EDITOR
    auto LiveIds = [&]
    {
        TArray<FString> Ids;
        World->GetSubsystem<UAngelIOTraceSubsystem>()->VisitExecuting(World, nullptr, [&](const FJsonObject& Call)
        {
            Ids.Add(Call.GetStringField(TEXT("operationId")));
        });
        return Ids;
    };
    TestTrue(TEXT("Executing trip remains inspectable after its call wraps"),
        LiveIds().Contains(LongRide.GetOperationId().ToString(EGuidFormats::DigitsWithHyphensLower)));
    LongRide.Finish();
    { FSharedAction Abandoned(World, World); }
    TestTrue(TEXT("Completed and released operations leave live target index"), LiveIds().IsEmpty());
#endif
    return true;
}
#endif
