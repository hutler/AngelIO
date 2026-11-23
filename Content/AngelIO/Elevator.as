// Each component owns its own globals. Inputs can change while Run is waiting.
int in_StartingFloor = 1;
bool in_Enabled = true;
float in_TravelSpeed = 240.0f;
int out_LastFloor;
int out_TotalTrips;
float out_TripSpeed;

enum InputSignal { RequestFloor, MotionFinished }
enum OutputSignal { MoveTo, Arrived }

void Run()
{
    out_LastFloor = in_StartingFloor;
    while (true)
    {
        WaitFor(InputSignal::RequestFloor);
        if (!in_Enabled)
            continue;

        int requested = SignalValue();
        out_TripSpeed = in_TravelSpeed;
        Emit(OutputSignal::MoveTo, requested);
        WaitFor(InputSignal::MotionFinished);
        out_LastFloor = SignalValue();
        out_TotalTrips++;
        Emit(OutputSignal::Arrived, out_LastFloor);
    }
}
