#include "AngelIOTargetRegistry.h"
#include "AngelIOScriptComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

TArray<UAngelIOScriptComponent*> UAngelIOTargetRegistry::FindTargets(FName TargetName) const
{
    TArray<UAngelIOScriptComponent*> Targets;
    if (const auto* Members = Groups.Find(TargetName))
        for (const auto& Member : *Members)
            if (auto* Target = Member.Get()) Targets.Add(Target);
    return Targets;
}

AActor* UAngelIOTargetRegistry::FindUniqueActor(FName TargetName, UClass* ActorClass) const
{
    AActor* Match = nullptr;
    if (const auto* Members = Groups.Find(TargetName))
    {
        for (const auto& Member : *Members)
        {
            const auto* Endpoint = Member.Get();
            auto* Owner = Endpoint ? Endpoint->GetOwner() : nullptr;
            if (!IsValid(Owner) || (ActorClass && !Owner->IsA(ActorClass))) continue;
            if (Match && Match != Owner) return nullptr;
            Match = Owner;
        }
    }
    return Match;
}

void UAngelIOTargetRegistry::Register(FName Name, UAngelIOScriptComponent* Target)
{
    if (!Name.IsNone()) Groups.FindOrAdd(Name).AddUnique(Target);
}

void UAngelIOTargetRegistry::Unregister(FName Name, UAngelIOScriptComponent* Target)
{
    if (auto* Members = Groups.Find(Name))
    {
        Members->RemoveAllSwap([Target](const auto& P) { return !P.IsValid() || P.Get() == Target; });
        if (Members->IsEmpty()) Groups.Remove(Name);
    }
}

int32 UAngelIOTargetRegistry::SendTo(FName TargetName, const FString& InputSignal, int32 Value)
{
    if (GetWorld()->GetNetMode() == NM_Client || InputSignal.IsEmpty()) return 0;
    auto* Members = Groups.Find(TargetName);
    if (!Members) return 0;
    Members->RemoveAllSwap([](const auto& P) { return !P.IsValid(); });
    const int32 Count = Members->Num();
    if (Count == 0) return 0;
    Queue.Add({*Members, InputSignal, Value});
    if (!bDelivering)
    {
        TGuardValue<bool> Guard(bDelivering, true);
        for (int32 Index = 0; Index < Queue.Num(); ++Index)
        {
            if (Index >= 1024)
            {
                UE_LOG(LogTemp, Error, TEXT("AngelIO: named signal feedback exceeded 1024 sends in one dispatch"));
                break;
            }
            // A callback can append to the queue, rename or destroy another recipient.
            FDelivery Delivery = MoveTemp(Queue[Index]);
            for (auto& Recipient : Delivery.Recipients)
                if (auto* Target = Recipient.Get(); Target && Target->HasBegunPlay())
                    Target->ReceiveNamedInput(Delivery.Input, Delivery.Value);
        }
        Queue.Reset();
    }
    return Count;
}
