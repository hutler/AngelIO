"""Observe both cooked TwoLifts arrivals, then inspect the closed standalone game's slot."""
import argparse
from contextlib import closing
from pathlib import Path
import sqlite3
import struct
import tempfile

from TestPackagedDelivery import run_game


def check(executable):
    with tempfile.TemporaryDirectory(prefix="AngelIO-packaged-") as temp:
        directory = Path(temp)
        with run_game(executable, directory, ["-AngelIODeliveryProbe=landings"]) as game:
            code = game.wait(timeout=90)
            if code:
                raise RuntimeError(f"Packaged game exited with code {code}")
        slot, = directory.rglob("slot-*.sqlite3")
        with closing(sqlite3.connect(slot.as_uri() + "?mode=ro", uri=True)) as db:
            rows = db.execute("select data from component_state where key=? and lower(map)=?",
                              ("angeliohost.liftlanding.v1", "/game/demo/twolifts"))
            floors = sorted(struct.unpack_from("<i", data)[0] for data, in rows)
        assert floors == [2, 3], floors
        print(f"{executable.name}: packaged TwoLifts committed floors {floors}")


if __name__ == "__main__":
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("executable", type=Path, help="Game executable under the staged project's Binaries/Win64")
    check(args.parse_args().executable.resolve(strict=True))
