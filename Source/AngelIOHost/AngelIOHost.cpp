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
    using FLiftHandle = TScriptHandle<TWeakObjectPtr<AAngelIOExampleLift>>;
    using FTripHandle = TScriptHandle<TSharedRef<FLiftTrip>>;

    void FindNamedLift(asIScriptGeneric* Call)
    {
        const auto& Name = *static_cast<const std::string*>(Call->GetArgObject(0));
        UWorld* World = AngelIO::GetScriptWorld();
        AAngelIOExampleLift* Match = nullptr;
        if (World && !Name.empty() && Name.size() < NAME_SIZE && Name.find('\0') == std::string::npos)
        {
            auto* Registry = World->GetSubsystem<UAngelIOTargetRegistry>();
            for (auto* Endpoint : Registry->FindTargets(FName(UTF8_TO_TCHAR(Name.c_str()), FNAME_Find)))
            {
                auto* Lift = Cast<AAngelIOExampleLift>(Endpoint->GetOwner());
                if (!IsValid(Lift)) continue;
                if (Match && Match != Lift) { Match = nullptr; break; }
                Match = Lift;
            }
        }
        Call->SetReturnAddress(Match ? new FLiftHandle(Match) : nullptr);
    }

    void MoveLift(asIScriptGeneric* Call)
    {
        auto* Lift = static_cast<FLiftHandle*>(Call->GetObject())->Value.Get();
        const int32 Floor = static_cast<int32>(Call->GetArgDWord(0));
        auto Trip = Lift ? Lift->MoveTo(Floor, Call->GetArgFloat(1)) : MakeShared<FLiftTrip>(Floor);
        if (!Lift) Trip->Finish(ELiftTripStatus::Cancelled);
        Call->SetReturnAddress(new FTripHandle(Trip));
    }

    void WaitForTrip(asIScriptGeneric* Call)
    {
        auto Trip = static_cast<FTripHandle*>(Call->GetObject())->Value;
        Call->SetReturnAddress(new FTripHandle(Trip));
        AngelIO::AwaitOperation(Trip, TEXT("LiftTrip.Wait"));
    }

    void TripStatus(asIScriptGeneric* Call)
    {
        Call->SetReturnDWord(static_cast<asDWORD>(static_cast<FTripHandle*>(Call->GetObject())->Value->Status));
    }

    void TripFloor(asIScriptGeneric* Call)
    {
        Call->SetReturnDWord(static_cast<FTripHandle*>(Call->GetObject())->Value->Floor);
    }
}

class FAngelIOHostModule : public FDefaultGameModuleImpl, public IAngelIOScriptAPI
{
public:
    void StartupModule() override { IModularFeatures::Get().RegisterModularFeature(FeatureName(), this); }
    void ShutdownModule() override { IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this); }
    bool SupportsDynamicReloading() override { return false; }

    bool RegisterBindings(asIScriptEngine& Engine) override
    {
        return FLiftHandle::Register(Engine, "Lift") && FTripHandle::Register(Engine, "LiftTrip") &&
            FTripHandle::Register(Engine, "TripResult") && Engine.RegisterEnum("TripStatus") >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Arrived", int(ELiftTripStatus::Arrived)) >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Rejected", int(ELiftTripStatus::Rejected)) >= 0 &&
            Engine.RegisterEnumValue("TripStatus", "Cancelled", int(ELiftTripStatus::Cancelled)) >= 0 &&
            Engine.RegisterGlobalFunction("Lift@ FindLift(const string &in name)", asFUNCTION(FindNamedLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("Lift", "LiftTrip@ MoveTo(int floor, float speed)", asFUNCTION(MoveLift), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("LiftTrip", "TripResult@ Wait() const", asFUNCTION(WaitForTrip), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("TripResult", "TripStatus get_Status() const property", asFUNCTION(TripStatus), asCALL_GENERIC) >= 0 &&
            Engine.RegisterObjectMethod("TripResult", "int get_Floor() const property", asFUNCTION(TripFloor), asCALL_GENERIC) >= 0;
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FAngelIOHostModule, AngelIOHost, "AngelIOHost");
