enum InputSignal { Activate }
bool out_Opened;

void Run()
{
    while (true)
    {
        WaitFor(InputSignal::Activate);
        Door@ door = FindDoor("Door");
        if (door is null)
        {
            out_Opened = false;
            continue;
        }
        DoorOpen@ opening = door.Open();
        opening.Wait();
        out_Opened = opening.Opened;
    }
}
