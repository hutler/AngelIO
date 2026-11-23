enum InputSignal { SelectFloor }

void Run()
{
    while (true)
    {
        WaitFor(InputSignal::SelectFloor);
        SendTo("DemoLifts", "RequestFloor", SignalValue());
    }
}
