enum InputSignal { Dispatch, Retry, Confirm, StopLamp }
enum Phase { Ready, Opening, Riding, AwaitConfirm, Complete, AwaitRetry }
enum Failure { None, DoorMissing, DoorFailed, LiftUnavailable, RideTimedOut, RideFailed }

int in_Floor = 4;
float in_Speed = 400.0f;
float in_Timeout = 8.0f;
int out_Phase;
int out_Failure;
bool out_Lamp;
int out_Credits;

void Blink()
{
    EndOn(InputSignal::StopLamp);
    while (true)
    {
        out_Lamp = !out_Lamp;
        WaitSeconds(0.2f);
    }
}

void StopBlinking()
{
    Notify(InputSignal::StopLamp);
    WaitFor(InputSignal::StopLamp);
    out_Lamp = false;
}

void AwaitRetry(Failure reason)
{
    StopBlinking();
    out_Failure = reason;
    out_Phase = Phase::AwaitRetry;
    WaitFor(InputSignal::Retry);
    out_Failure = Failure::None;
    Fork(Blink);
}

void Run()
{
    string stage = RequireCheckpoint(1);
    if (stage == "")
    {
        WaitFor(InputSignal::Dispatch);
        SaveCheckpoint("Opening", 1);
        stage = "Opening";
    }
    if (stage == "Opening" || stage == "Dispatched")
        Fork(Blink);
    while (stage == "Opening")
    {
        out_Phase = Phase::Opening;
        // A restored terminal can begin play before the room's door registers.
        WaitSeconds(0.01f);
        Door@ door = FindDoor("Door");
        if (door is null) { AwaitRetry(Failure::DoorMissing); continue; }
        if (!door.IsOpen)
        {
            DoorOpen@ opening = door.Open();
            opening.Wait();
            if (!opening.Opened) { AwaitRetry(Failure::DoorFailed); continue; }
        }
        SaveCheckpoint("Dispatched", 1);
        stage = "Dispatched";
    }
    while (stage == "Dispatched")
    {
        out_Phase = Phase::Riding;
        Lift@ lift = WaitActor("Lift", in_Timeout);
        if (lift is null) { AwaitRetry(Failure::LiftUnavailable); continue; }
        if (!lift.HasSavedArrival(in_Floor))
        {
            TripResult@ ride = lift.MoveTo(in_Floor, in_Speed).Wait(in_Timeout);
            if (ride.Status != TripStatus::Arrived)
            {
                AwaitRetry(ride.Status == TripStatus::TimedOut ? Failure::RideTimedOut : Failure::RideFailed);
                continue;
            }
        }
        SaveCheckpoint("Delivered", 1);
        stage = "Delivered";
        StopBlinking();
    }
    if (stage == "Delivered")
    {
        out_Phase = Phase::AwaitConfirm;
        WaitFor(InputSignal::Confirm);
        SaveCheckpoint("Confirming", 1);
        stage = "Confirming";
    }
    if (stage == "Confirming" || stage == "Complete")
    {
        // Includes confirmations written before the credit was introduced.
        RecordOnce("DeliveryCredit");
        if (stage == "Confirming") SaveCheckpoint("Complete", 1);
        out_Credits = 1;
        out_Phase = Phase::Complete;
    }
}
