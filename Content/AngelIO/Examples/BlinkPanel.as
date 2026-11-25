enum InputSignal { Dispatch, RideEnded, Confirm }
bool out_Lamp;
int out_Pulses;

void Blink()
{
    EndOn(InputSignal::RideEnded);
    while (true)
    {
        out_Lamp = !out_Lamp;
        out_Pulses++;
        WaitSeconds(0.1f);
    }
}

void Run()
{
    WaitFor(InputSignal::Dispatch);
    Lift@ lift = WaitActor("FreightLift");
    Fork(Blink);
    TripResult@ result = lift.MoveTo(4, 400.0f).Wait();
    Notify(InputSignal::RideEnded);
    // A due blink can still run in this tick before the queued signal is delivered.
    WaitFor(InputSignal::RideEnded);
    out_Lamp = false;
    if (result.Status == TripStatus::Arrived)
        WaitFor(InputSignal::Confirm);
}
