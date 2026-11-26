"""Exercise both delivery rooms and recover them on a fresh packaged listen server and client."""
import argparse
from contextlib import contextmanager, closing
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time


@contextmanager
def run_game(executable, directory, extra):
    directory.mkdir(parents=True, exist_ok=True)
    flags = ["-nullrhi", "-nosound", "-unattended", "-nosplash", "-windowed", "-ResX=320", "-ResY=200",
             "-SaveToUserDir", f"-UserDir={directory}"]
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = subprocess.SW_HIDE
    process = subprocess.Popen([str(executable), *flags, *extra], cwd=executable.parent, startupinfo=info)
    try:
        yield process
    finally:
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=15)


def check_alive(process, role):
    if process.poll() is not None:
        raise RuntimeError(f"{role} exited early with code {process.returncode}")


def wait_for_host(process, port):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        check_alive(process, "Host")
        rows = subprocess.check_output(["netstat", "-ano", "-p", "udp"], creationflags=subprocess.CREATE_NO_WINDOW)
        if any(len(cols) == 4 and cols[0] == b"UDP" and cols[1].endswith(f":{port}".encode())
               and cols[-1] == str(process.pid).encode() for cols in map(bytes.split, rows.splitlines())):
            return
        time.sleep(.25)
    raise TimeoutError("Listen host did not bind its game port")


def wait_for_client(client, host, directory):
    deadline = time.monotonic() + 90
    last = "No client observation"
    while time.monotonic() < deadline:
        check_alive(host, "Host")
        check_alive(client, "Client")
        for marker in directory.rglob("AngelIODeliveryProbe.txt"):
            last = marker.read_text(encoding="utf-8")
            if last == "PASS":
                return
        time.sleep(.2)
    raise TimeoutError(last)


def read_slot(directory, restore):
    slot, = directory.rglob("slot-Packaging.sqlite3")
    with closing(sqlite3.connect(slot.as_uri() + "?mode=ro", uri=True)) as db:
        progress = dict(db.execute("select owner,stage from checkpoints"))
        expected = b"Complete" if restore else b"Delivered"
        assert len(progress) == 2 and all(stage == expected for stage in progress.values()), progress
        landings = dict(db.execute("select owner,data from component_state where key=?", ("angeliohost.liftlanding.v1",)))
        assert len(landings) == 2 and all(struct.unpack_from("<i", data)[0] == 4 for data in landings.values()), landings
        credits = db.execute("select owner,id from action_receipts where action='DeliveryCredit'").fetchall()
        assert len(credits) == (2 if restore else 0), credits
        if restore:
            assert set(progress) == {owner for owner, _ in credits} and len({receipt for _, receipt in credits}) == 2
        return landings


def check(executable):
    with tempfile.TemporaryDirectory(prefix="AngelIO-delivery-") as tmp:
        root = Path(tmp)
        original = None
        for phase in ("dispatch", "restore"):
            with closing(socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as port_probe:
                port_probe.bind(("127.0.0.1", 0))
                port = port_probe.getsockname()[1]
            host_dir, client_dir = root / "host", root / phase
            with run_game(executable, host_dir, ["-AngelIODeliveryProbe=host", f"-port={port}"]) as host:
                wait_for_host(host, port)
                with run_game(executable, client_dir, [f"-AngelIODeliveryProbe={phase}", f"-AngelIODeliveryPeerPort={port}"]) as client:
                    wait_for_client(client, host, client_dir)
            landings = read_slot(host_dir, phase == "restore")
            if original is None:
                original = landings
            else:
                assert landings == original, "The recovered ride rewrote a landing"
            print(f"{executable.name} {phase}: both rooms passed on a separate client and listen process", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    check(parser.parse_args().executable.resolve(strict=True))
