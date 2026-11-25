enum InputSignal { Dispatch, Confirm }
int out_Stage; // 0 = awaiting delivery, 1 = delivered, 2 = complete

void Run()
{
    string stage = LoadCheckpoint(1);
    if (stage == "")
    {
        WaitFor(InputSignal::Dispatch);
        SaveCheckpoint("Dispatched", 1);
        stage = "Dispatched";
    }
    if (stage == "Dispatched")
    {
        Lift@ lift = WaitActor("FreightLift");
        // The landing for this dispatch can reach disk before the mission saves Delivered.
        if (!lift.HasSavedArrival(4) && lift.MoveTo(4, 800.0f).Wait().Status != TripStatus::Arrived)
            return;
        SaveCheckpoint("Delivered", 1);
        stage = "Delivered";
    }
    if (stage == "Delivered")
    {
        out_Stage = 1;
        WaitFor(InputSignal::Confirm);
        SaveCheckpoint("Complete", 1);
        stage = "Complete";
    }
    if (stage == "Complete")
        out_Stage = 2;
}
