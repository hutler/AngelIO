"""Launch a staged Win64 game and verify the two demo landings in an isolated save directory."""
import argparse
from contextlib import closing
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import time


def check(executable):
    with tempfile.TemporaryDirectory(prefix="AngelIO-packaged-") as temp:
        user_dir = Path(temp)
        options = subprocess.STARTUPINFO()
        options.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        options.wShowWindow = subprocess.SW_HIDE
        process = subprocess.Popen([
            str(executable), "/Game/Demo/TwoLifts?listen?AngelIOCheckpointSlot=Packaging",
            f"-UserDir={user_dir}", "-SaveToUserDir", "-nullrhi", "-unattended",
            "-nosound", "-nosplash", "-windowed", "-ResX=320", "-ResY=200",
        ], cwd=executable.parent, startupinfo=options)
        try:
            deadline = time.monotonic() + 90
            last_read_error = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Packaged game exited early with code {process.returncode}")
                for slot in user_dir.rglob("slot-*.sqlite3"):
                    with closing(sqlite3.connect(slot.as_uri() + "?mode=ro", uri=True)) as db:
                        try:
                            rows = db.execute("select map, data from component_state where key=?",
                                              ("angeliohost.liftlanding.v1",)).fetchall()
                        except sqlite3.OperationalError as exc:
                            # The live writer may still be creating the schema or committing its journal.
                            code = exc.sqlite_errorcode & 0xff
                            if code not in (sqlite3.SQLITE_READONLY, sqlite3.SQLITE_BUSY, sqlite3.SQLITE_LOCKED) and "no such table" not in str(exc):
                                raise
                            last_read_error = exc
                            continue
                    floors = sorted(struct.unpack_from("<i", data)[0] for map_name, data in rows
                                    if map_name.lower() == "/game/demo/twolifts")
                    if floors == [2, 3]:
                        print(f"{executable.name}: packaged TwoLifts committed floors {floors}")
                        return
                time.sleep(0.2)
            raise TimeoutError("The packaged game did not commit both demo landings") from last_read_error
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=15)


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("executable", type=Path, help="Game executable under the staged project's Binaries/Win64")
    check(args.parse_args().executable.resolve(strict=True))
