#!/usr/bin/env python3
"""Durable, metadata-only audit of explicitly invoked task network operations."""

import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid


DEFAULT_LOG = Path(__file__).resolve().parents[3] / "out/codex-network-operations.jsonl"
FIELDS = (
    "kind", "target", "purpose", "operation", "data_sent", "authorization",
    "network_scope", "risk", "risk_reason", "parent_id",
)


def redact(value):
    """Defense in depth only; callers must never supply inline credentials."""
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, dict):
        return {key: redact(item) for key, item in value.items()}
    if not isinstance(value, str):
        return value
    value = re.sub(r"(https?://)[^/\s@]+@", r"\1[REDACTED]@", value)
    value = re.sub(
        r"(?i)(\b(?:authorization|proxy-authorization|cookie|set-cookie)\s*:\s*)"
        r"[^\r\n]+", r"\1[REDACTED]", value,
    )
    value = re.sub(r"(?i)(\bbearer\s+)\S+", r"\1[REDACTED]", value)
    return re.sub(
        r"(?i)((?:--|\b)(?:password|passwd|token|access_token|api[_-]?key|secret)"
        r"(?:=|\s+))(?:(?:\"[^\"]*\")|(?:'[^']*')|[^&\s]+)",
        r"\1[REDACTED]", value,
    )


def record_event(path, record, require_open=False):
    """Append and sync under one lock; audit errors prevent command execution."""
    flags = os.O_RDWR | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "a+", encoding="utf-8") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("audit destination must be a regular file")
        os.fchmod(stream.fileno(), 0o600)
        stream.seek(0)
        previous = [json.loads(line) for line in stream if line.strip()]
        if require_open:
            matches = [item for item in previous if item.get("id") == record["id"]]
            if not matches or matches[-1].get("phase") != "begin":
                raise ValueError("operation ID has no unfinished begin record")
        event = {
            "schema_version": 1,
            "recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            **record,
        }
        stream.write(json.dumps(redact(event), ensure_ascii=False) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    return event


def begin(args, argv=None):
    event = {key: getattr(args, key) for key in FIELDS}
    event.update(id=str(uuid.uuid4()), phase="begin")
    if argv is not None:
        # Redact as one command first, so separate --password VALUE is covered.
        import shlex
        event["command"] = redact(shlex.join(argv))
    return record_event(args.log, event)["id"]


def finish(args, operation_id, status, details, exit_code=None):
    event = {
        "id": operation_id, "phase": "end", "status": status,
        "details": details, "exit_code": exit_code,
    }
    return record_event(args.log, event, require_open=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    commands = parser.add_subparsers(dest="action", required=True)
    for name in ("begin", "run"):
        command = commands.add_parser(name)
        for field in FIELDS:
            options = {"required": field != "parent_id"}
            if field == "risk":
                options["choices"] = ("low", "review", "high", "unknown")
            command.add_argument("--" + field.replace("_", "-"), **options)
        if name == "run":
            command.add_argument("command", nargs=argparse.REMAINDER)
    end = commands.add_parser("finish")
    end.add_argument("--id", required=True)
    end.add_argument("--status", choices=("completed", "failed", "unknown"), required=True)
    end.add_argument("--details", required=True)
    end.add_argument("--exit-code", type=int)
    commands.add_parser("status")
    args = parser.parse_args(argv)
    try:
        if args.action == "status":
            events = []
            if args.log.exists():
                with args.log.open(encoding="utf-8") as stream:
                    fcntl.flock(stream.fileno(), fcntl.LOCK_SH)
                    events = [json.loads(line) for line in stream if line.strip()]
            latest = {}
            for event in events:
                latest[event["id"]] = event
            print(json.dumps({
                "events": len(events), "operations": len(latest),
                "unfinished": [item for item in latest.values()
                               if item.get("phase") == "begin"],
                "unknown": [item for item in latest.values()
                            if item.get("status") == "unknown"],
            }, ensure_ascii=False))
            return 0
        if args.action == "finish":
            finish(args, args.id, args.status, args.details, args.exit_code)
            return 0
        if args.action == "begin":
            print(begin(args), flush=True)
            return 0
        command = args.command
        if command and command[0] == "--":
            command = command[1:]
        if not command:
            parser.error("run requires a command after --")
        operation_id = begin(args, command)
        print("network-audit id=" + operation_id, file=sys.stderr, flush=True)
        try:
            result = subprocess.run(command, check=False)
        except OSError as error:
            finish(args, operation_id, "failed", type(error).__name__, 127)
            return 127
        except KeyboardInterrupt:
            finish(args, operation_id, "unknown", "interrupted; inspect original job")
            return 130
        code = result.returncode
        finish(args, operation_id, "completed" if code == 0 else "failed",
               "command exited; output not copied into audit log", code)
        return code if code >= 0 else 128 - code
    except (OSError, ValueError) as error:
        print("network-audit error: " + str(error), file=sys.stderr)
        return 78


if __name__ == "__main__":
    sys.exit(main())
