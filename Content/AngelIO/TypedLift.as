enum InputSignal { CallLift }
float in_Speed = 300.0f;
int out_LastFloor;
int out_LastStatus = -1;
int out_Completed;

void Run()
{
    while (true)
    {
        WaitFor(InputSignal::CallLift);
        int floor = SignalValue();
        Lift@ lift = FindLift("FreightLift");
        if (lift is null)
        {
            out_LastStatus = int(TripStatus::Rejected);
            out_LastFloor = floor;
        }
        else
        {
            LiftTrip@ trip = lift.MoveTo(floor, in_Speed);
            TripResult@ result = trip.Wait();
            out_LastStatus = int(result.Status);
            out_LastFloor = result.Floor;
        }
        out_Completed++;
    }
}
