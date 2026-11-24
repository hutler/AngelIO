enum InputSignal { Dispatch, Confirm }
int out_Stage; // 0 = awaiting dispatch, 1 = delivered, 2 = complete

void Run()
{
    string stage = LoadCheckpoint(1);
    if (stage == "")
    {
        WaitFor(InputSignal::Dispatch);
        Lift@ lift = WaitActor("FreightLift");
        TripResult@ ride = lift.MoveTo(4, 800.0f).Wait();
        if (ride.Status != TripStatus::Arrived)
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
