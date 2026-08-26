#!/usr/bin/env python3

import argparse
import pathlib
import signal
import sys
import time


stopping = False


def request_stop(_signum, _frame):
    global stopping
    stopping = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--nodes", type=int, choices=(2, 4, 8), default=4)
    parser.add_argument("--delay-ms", type=int, default=80)
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, request_stop)
    run_dir = pathlib.Path("guest-linux/aarch64/logs") / f"{args.run_id}_fixture"
    run_dir.mkdir(parents=True, exist_ok=True)
    logs = []
    for index in range(args.nodes):
        node = f"node{chr(ord('A') + index)}"
        path = run_dir / f"{node}_guest.log"
        path.write_text(f"[fixture] {node} booting\n", encoding="utf-8")
        logs.append((node, path))

    print(f"fixture cluster starting nodes={args.nodes}", flush=True)
    for step in range(4):
        if stopping:
            print("fixture cluster stopped", flush=True)
            return 143
        for node, path in logs:
            with path.open("a", encoding="utf-8") as output:
                output.write(f"[fixture] {node} ready step={step}\n")
        print(f"fixture progress step={step + 1}/4", flush=True)
        time.sleep(args.delay_ms / 1000)

    for node, path in logs:
        with path.open("a", encoding="utf-8") as output:
            output.write(f"[fixture] {node} verdict=PASS\n")
    print("fixture cluster pass", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
