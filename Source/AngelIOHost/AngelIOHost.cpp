#include "Modules/ModuleManager.h"
#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIORoomDoor.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "AngelIOTraceSubsystem.h"
#include "Dom/JsonObject.h"
#include "Features/IModularFeatures.h"
#include <string>

THIRD_PARTY_INCLUDES_START
#include "angelscript.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    template<class T>
    struct TScriptHandle
    {
        explicit TScriptHandle(T InValue) : Value(MoveTemp(InValue)) {}
        T Value;
        int32 References = 1;

        static void AddRef(asIScriptGeneric* Call)
        {
            ++static_cast<TScriptHandle*>(Call->GetObject())->References;
        }
        static void Release(asIScriptGeneric* Call)
        {
            auto* Self = static_cast<TScriptHandle*>(Call->GetObject());
            if (--Self->References == 0) delete Self;
        }
        static bool Register(asIScriptEngine& Engine, const char* Name)
        {
            return Engine.RegisterObjectType(Name, 0, asOBJ_REF) >= 0 &&
                Engine.RegisterObjectBehaviour(Name, asBEHAVE_ADDREF, "void f()", asFUNCTION(AddRef), asCALL_GENERIC) >= 0 &&
                Engine.RegisterObjectBehaviour(Name, asBEHAVE_RELEASE, "void f()", asFUNCTION(Release), asCALL_GENERIC) >= 0;
        }
    };
    template<class TActor>
    struct TNamedActor
    {
        TWeakObjectPtr<TActor> Actor;
        FName Target;
        uint64 PlayGeneration;

        TActor* Get() const
        {
            auto* Current = Actor.Get();
            return Current && Current->GetPlayGeneration() == PlayGeneration ? Current : nullptr;
        }
    };
    using FLiftHandle = TScriptHandle<TNamedActor<AAngelIOExampleLift>>;
    using FDoorHandle = TScriptHandle<TNamedActor<AAngelIORoomDoor>>;
    using FDoorOpenHandle = TScriptHandle<TSharedRef<FDoorOpen>>;
    // Script TripStatus values 0..2 are producer results. Value 3 belongs to the waiter.
    constexpr asDWORD TimedOutResult = 3;
    struct FTripReference
    {
        TSharedRef<FLiftTrip> Trip;
        TSharedPtr<const EAngelIOWaitOutcome> Wait;
    };
    using FTripHandle = TScriptHandle<FTripReference>;

    FString ReadName(asIScriptGeneric* Call)
    {
        const auto& Name = *static_cast<const std::string*>(Call->GetArgObject(0));
        if (Name.empty() || Name.size() >= NAME_SIZE || Name.find('\0') != std::string::npos) return {};
        return UTF8_TO_TCHAR(Name.c_str());
    }

    struct FActorLookup
    {
        FString Name;
        TWeakObjectPtr<ULevel> Room;
        bool bLocal = false;
    };

    FActorLookup ActorLookup(const FString& Name)
    {
        if (auto* Mission = AngelIO::GetScriptMission())
            if (const FName* Bound = Mission->RoomTargets.Find(FName(*Name, FNAME_Find)))
                return {Bound->ToString(), Mission->GetOwner()->GetLevel(), true};
        return {Name, nullptr, false};
    }

    template<class TActor>
    TActor* ResolveActor(UWorld* World, const FActorLookup& Lookup)
    {
        if (!World || Lookup.Name.IsEmpty() || (Lookup.bLocal && !Lookup.Room.IsValid())) return nullptr;
        auto* Registry = World->GetSubsystem<UAngelIOTargetRegistry>();
        return Cast<TActor>(Registry->FindUniqueActor(FName(*Lookup.Name, FNAME_Find), TActor::StaticClass(), Lookup.Room.Get()));
    }

    AAngelIOExampleLift* ResolveLift(UWorld* World, const FActorLookup& Lookup, bool bRequireReady)
    {
        auto* Lift = ResolveActor<AAngelIOExampleLift>(World, Lookup);
        if (Lift && bRequireReady && !Lift->IsReadyForTrip())
            return nullptr;
        return Lift;
    }

    void ReturnLift(asIScriptGeneric* Call, bool bRequireReady)
    {
        const FString Name = ReadName(Call);
        const auto Lookup = ActorLookup(Name);
        auto* Lift = ResolveLift(AngelIO::GetScriptWorld(), Lookup, bRequireReady);
        Call->SetReturnAddress(Lift ? new FLiftHandle({Lift, FName(*Lookup.Name), Lift->GetPlayGeneration()}) : nullptr);
    }
    void FindNamedLift(asIScriptGeneric* Call) { ReturnLift(Call, false); }
    void FindReadyLift(asIScriptGeneric* Call) { ReturnLift(Call, true); }

    void WaitForLift(asIScriptGeneric* Call)
    {
        const FString Name = ReadName(Call);
        if (Name.IsEmpty() || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            asGetActiveContext()->SetException("WaitActor requires a valid target name");
            return;
        }
        TOptional<double> Timeout;
        if (Call->GetArgCount() == 2) Timeout = Call->GetArgFloat(1);
        TWeakObjectPtr<UWorld> World = AngelIO::GetScriptWorld();
        const auto Lookup = ActorLookup(Name);
        AngelIO::AwaitCondition(TEXT("WaitActor"), Lookup.Name, Timeout,
            [World, Lookup] { return ResolveLift(World.Get(), Lookup, true) != nullptr; });
    }

    void FindDoor(asIScriptGeneric* Call)
    {
        const FString Role = ReadName(Call);
        const auto Lookup = ActorLookup(Role);
        auto* World = AngelIO::GetScriptWorld();
        auto* Door = ResolveActor<AAngelIORoomDoor>(World, Lookup);
        if (auto* Trace = !Door && World ? World->GetSubsystem<UAngelIOTraceSubsystem>() : nullptr)
        {
            TSet<AActor*> Candidates;
            int32 Matching = 0;
            for (auto* Endpoint : World->GetSubsystem<UAngelIOTargetRegistry>()->FindTargets(FName(*Lookup.Name, FNAME_Find)))
                if (auto* Owner = Endpoint->GetOwner(); IsValid(Owner) && (!Lookup.bLocal || Owner->GetLevel() == Lookup.Room.Get()) &&
                    !Candidates.Contains(Owner))
                {
                    Candidates.Add(Owner);
                    if (Owner->IsA<AAngelIORoomDoor>()) ++Matching;
                }
            auto Args = MakeShared<FJsonObject>();
            Args->SetStringField(TEXT("role"), Role);
            Args->SetStringField(TEXT("expectedClass"), AAngelIORoomDoor::StaticClass()->GetPathName());
            Args->SetNumberField(TEXT("candidates"), Candidates.Num());
            Args->SetStringField(TEXT("scope"), Lookup.bLocal ? GetPathNameSafe(Lookup.Room.Get()) : TEXT("world"));
            Trace->RecordDecision(TEXT("FindDoor"), Lookup.Name, Matching > 1 ? TEXT("Ambiguous") :
                Candidates.IsEmpty() ? TEXT("Missing") : TEXT("WrongClass"), AngelIO::CaptureTraceSite(), AngelIO::GetScriptMission(), Args);
        }
        Call->SetReturnAddress(Door ? new FDoorHandle({Door, FName(*Lookup.Name), Door->GetPlayGeneration()}) : nullptr);
    }

    void OpenDoor(asIScriptGeneric* Call)
    {
        const auto& Handle = static_cast<FDoorHandle*>(Call->GetObject())->Value;
        auto* Door = Handle.Get();
        auto Request = Door ? Door->Open(Handle.Target) :
            MakeShared<FDoorOpen>(AngelIO::GetScriptWorld(), Handle.Target, nullptr);
        if (!Door) Request->Finish(false, TEXT("TargetGone"));
        Call->SetReturnAddress(new FDoorOpenHandle(Request));
    }

    void IsDoorOpen(asIScriptGeneric* Call)
    {
        const auto* Door = static_cast<FDoorHandle*>(Call->GetObject())->Value.Get();
        Call->SetReturnByte(Door && Door->IsOpen());
    }

    void WaitForDoor(asIScriptGeneric* Call)
    {
        AngelIO::AwaitOperation(static_cast<FDoorOpenHandle*>(Call->GetObject())->Value, TEXT("DoorOpen.Wait"));
    }

    void DoorOpened(asIScriptGeneric* Call)
    {
        Call->SetReturnByte(static_cast<FDoorOpenHandle*>(Call->GetObject())->Value->bOpened);
    }

    void HasSavedArrival(asIScriptGeneric* Call)
    {
        const auto& Handle = static_cast<FLiftHandle*>(Call->GetObject())->Value;
        const auto* Lift = Handle.Get();
        const int32 Floor = static_cast<int32>(Call->GetArgDWord(0));
        const FGuid Step = AngelIO::GetCheckpointStepId();
        const bool Arrived = Lift && Lift->HasSavedArrival(Floor, Step);
        if (auto* World = AngelIO::GetScriptWorld())
            if (auto* Trace = World->GetSubsystem<UAngelIOTraceSubsystem>())
            {
                auto Args = MakeShared<FJsonObject>();
                Args->SetNumberField(TEXT("floor"), Floor);
                if (Step.IsValid()) Args->SetStringField(TEXT("stepId"), Step.ToString(EGuidFormats::DigitsWithHyphensLower));
                Trace->RecordDecision(TEXT("Lift.HasSavedArrival"), Handle.Target.ToString(),
                    Arrived ? TEXT("ArrivalCommitted") : TEXT("NoMatchingArrival"), AngelIO::CaptureTraceSite(), Lift, Args);
            }
        Call->SetReturnByte(Arrived);
    }

    void MoveLift(asIScriptGeneric* Call)
    {
        const auto& Handle = static_cast<FLiftHandle*>(Call->GetObject())->Value;
        auto* Lift = Handle.Get();
        const int32 Floor = static_cast<int32>(Call->GetArgDWord(0));
        const float Speed = Call->GetArgFloat(1);
        auto Trip = Lift ? Lift->MoveTo(Floor, Speed, Handle.Target, AngelIO::GetCheckpointStepId()) :
            MakeShared<FLiftTrip>(AngelIO::GetScriptWorld(), Handle.Target.ToString(), Floor, Speed);
        if (!Lift) Trip->Finish(ELiftTripStatus::Cancelled, TEXT("TargetGone"));
        Call->SetReturnAddress(new FTripHandle({Trip, nullptr}));
    }

    void WaitForTrip(asIScriptGeneric* Call)
    {
        auto Trip = static_cast<FTripHandle*>(Call->GetObject())->Value.Trip;
        TOptional<double> Timeout;
        if (Call->GetArgCount()) Timeout = Call->GetArgFloat(0);
        auto Wait = AngelIO::AwaitOperation(Trip, TEXT("LiftTrip.Wait"), Timeout);
        Call->SetReturnAddress(new FTripHandle({Trip, Wait}));
    }

    void TripStatus(asIScriptGeneric* Call)
    {
        const auto& Result = static_cast<FTripHandle*>(Call->GetObject())->Value;
        Call->SetReturnDWord(*Result.Wait == EAngelIOWaitOutcome::TimedOut ?
            TimedOutResult : static_cast<asDWORD>(Result.Trip->Status));
    }

    void TripId(asIScriptGeneric* Call)
    {
        const auto& Id = static_cast<FTripHandle*>(Call->GetObject())->Value.Trip->GetOperationId();
        const FString String = Id.ToString(EGuidFormats::DigitsWithHyphensLower);
        new (Call->GetAddressOfReturnLocation()) std::string(TCHAR_TO_UTF8(*String));
    }

    void TripFloor(asIScriptGeneric* Call)
    {
        Call->SetReturnDWord(static_cast<FTripHandle*>(Call->GetObject())->Value.Trip->Floor);
    }
}

FDelegateHandle StartAngelIOPackagedProbe();

class FAngelIOHostModule : public FDefaultGameModuleImpl, public IAngelIOScriptAPI
{
public:
    void StartupModule() override
    {
        IModularFeatures::Get().RegisterModularFeature(FeatureName(), this);
        PackagedProbe = StartAngelIOPackagedProbe();
    }
    void ShutdownModule() override
    {
        FWorldDelegates::OnWorldTickStart.Remove(PackagedProbe);
        IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this);
    }
private:
    FDelegateHandle PackagedProbe;
public:
    bool SupportsDynamicReloading() override { return false; }
    void GetScriptWrappers(TArray<FAngelIOScriptWrapper>& Out) const override
    {
        Out.Add({TEXT("Lift@ WaitActor(const string &in name)"),
            TEXT("AngelIOInternal::WaitForLift(name); return AngelIOInternal::FindReadyLift(name);")});
        Out.Add({TEXT("Lift@ WaitActor(const string &in name, float timeoutSeconds)"),
            TEXT("AngelIOInternal::WaitForLift(name, timeoutSeconds); return AngelIOInternal::FindReadyLift(name);")});
    }
#if WITH_EDITOR
    void GetMapLookups(TArray<FAngelIOMapLookup>& Out) const override
    {
        Out.Add({TEXT("FindLift"), AAngelIOExampleLift::StaticClass(), true});
        Out.Add({TEXT("WaitActor"), AAngelIOExampleLift::StaticClass(), true, true});
        Out.Add({TEXT("FindDoor"), AAngelIORoomDoor::StaticClass(), true});
    }
#endif

    bool RegisterBindings(asIScriptEngine& Engine) override
    {
        return FDoorHandle::Register(Engine, "Door") && FDoorOpenHandle::Register(Engine, "DoorOpen") &&
            Engine.RegisterGlobalFunction("Door@ FindDoor(const string &in name)", asFUNCTION(FindDoor), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("Door", "DoorOpen@ Open()", asFUNCTION(OpenDoor), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("Door", "bool get_IsOpen() const property", asFUNCTION(IsDoorOpen), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("DoorOpen", "void Wait()", asFUNCTION(WaitForDoor), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("DoorOpen", "bool get_Opened() const property", asFUNCTION(DoorOpened), asCALL_GENERIC) >= 0 &&
            FLiftHandle::Register(Engine, "Lift") && FTripHandle::Register(Engine, "LiftTrip") &&
            FTripHandle::Register(Engine, "TripResult") && Engine.RegisterEnum("TripStatus") >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Arrived", int(ELiftTripStatus::Arrived)) >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Rejected", int(ELiftTripStatus::Rejected)) >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Cancelled", int(ELiftTripStatus::Cancelled)) >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "TimedOut", TimedOutResult) >= 0 &&
            Engine.RegisterGlobalFunction("Lift@ FindLift(const string &in name)", asFUNCTION(FindNamedLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("Lift", "LiftTrip@ MoveTo(int floor, float speed)", asFUNCTION(MoveLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("Lift", "bool HasSavedArrival(int floor) const", asFUNCTION(HasSavedArrival), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("LiftTrip", "TripResult@ Wait() const", asFUNCTION(WaitForTrip), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("LiftTrip", "TripResult@ Wait(float timeoutSeconds) const", asFUNCTION(WaitForTrip), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("LiftTrip", "string get_Id() const property", asFUNCTION(TripId), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("TripResult", "string get_Id() const property", asFUNCTION(TripId), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("TripResult", "TripStatus get_Status() const property", asFUNCTION(TripStatus), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("TripResult", "int get_Floor() const property", asFUNCTION(TripFloor), asCALL_GENERIC) >= 0 &&
            Engine.SetDefaultNamespace("AngelIOInternal") >= 0 &&
            Engine.RegisterGlobalFunction("void WaitForLift(const string &in)", asFUNCTION(WaitForLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterGlobalFunction("void WaitForLift(const string &in, float)", asFUNCTION(WaitForLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterGlobalFunction("::Lift@ FindReadyLift(const string &in)", asFUNCTION(FindReadyLift), asCALL_GENERIC) >= 0 &&
            Engine.SetDefaultNamespace("") >= 0;
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FAngelIOHostModule, AngelIOHost, "AngelIOHost");
