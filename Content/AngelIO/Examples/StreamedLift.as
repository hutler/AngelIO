enum InputSignal { CallLift }

float in_TimeoutSeconds = 10.0f;
float in_Speed = 800.0f;
int out_Completed;
int out_LastStatus = -1;
int out_LastFloor;

void Run()
{
    while (true)
    {
        WaitFor(InputSignal::CallLift);
        int floor = SignalValue();
        Lift@ lift = WaitActor("FreightLift", in_TimeoutSeconds);
        if (lift is null)
            out_LastStatus = int(TripStatus::TimedOut);
        else
        {
            TripResult@ ride = lift.MoveTo(floor, in_Speed).Wait();
            out_LastStatus = int(ride.Status);
            out_LastFloor = ride.Floor;
        }
        out_Completed++;
    }
}
