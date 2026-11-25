#include "Modules/ModuleManager.h"
#include "AngelIOExampleLift.h"
#include "AngelIOLiftTrip.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOTargetRegistry.h"
#include "Engine/World.h"
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
    struct FNamedLift
    {
        TWeakObjectPtr<AAngelIOExampleLift> Actor;
        FName Target;
        uint64 PlayGeneration;
    };
    using FLiftHandle = TScriptHandle<FNamedLift>;
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

    AAngelIOExampleLift* ResolveLift(UWorld* World, const FString& Name, bool bRequireReady)
    {
        if (!World || Name.IsEmpty()) return nullptr;
        auto* Registry = World->GetSubsystem<UAngelIOTargetRegistry>();
        auto* Lift = Cast<AAngelIOExampleLift>(Registry->FindUniqueActor(FName(*Name, FNAME_Find), AAngelIOExampleLift::StaticClass()));
        if (Lift && bRequireReady && !Lift->IsReadyForTrip())
            return nullptr;
        return Lift;
    }

    void ReturnLift(asIScriptGeneric* Call, bool bRequireReady)
    {
        const FString Name = ReadName(Call);
        auto* Lift = ResolveLift(AngelIO::GetScriptWorld(), Name, bRequireReady);
        Call->SetReturnAddress(Lift ? new FLiftHandle({Lift, FName(*Name), Lift->GetPlayGeneration()}) : nullptr);
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
        AngelIO::AwaitCondition(TEXT("WaitActor"), Name, Timeout,
            [World, Name] { return ResolveLift(World.Get(), Name, true) != nullptr; });
    }

    AAngelIOExampleLift* GetCurrentLift(const FNamedLift& Handle)
    {
        auto* Lift = Handle.Actor.Get();
        return Lift && Lift->GetPlayGeneration() == Handle.PlayGeneration ? Lift : nullptr;
    }

    void HasSavedArrival(asIScriptGeneric* Call)
    {
        const auto* Lift = GetCurrentLift(static_cast<FLiftHandle*>(Call->GetObject())->Value);
        Call->SetReturnByte(Lift && Lift->HasSavedArrival(static_cast<int32>(Call->GetArgDWord(0)), AngelIO::GetCheckpointStepId()));
    }

    void MoveLift(asIScriptGeneric* Call)
    {
        const auto& Handle = static_cast<FLiftHandle*>(Call->GetObject())->Value;
        auto* Lift = GetCurrentLift(Handle);
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

class FAngelIOHostModule : public FDefaultGameModuleImpl, public IAngelIOScriptAPI
{
public:
    void StartupModule() override { IModularFeatures::Get().RegisterModularFeature(FeatureName(), this); }
    void ShutdownModule() override { IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this); }
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
    }
#endif

    bool RegisterBindings(asIScriptEngine& Engine) override
    {
        return FLiftHandle::Register(Engine, "Lift") && FTripHandle::Register(Engine, "LiftTrip") &&
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
