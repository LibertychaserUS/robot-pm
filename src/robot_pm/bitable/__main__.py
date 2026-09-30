"""Command line for the Bitable editor.

    python -m robot_pm.bitable apply path/to/edit.json --dry-run
"""

from __future__ import annotations

import argparse
import json
import sys

from robot_pm.bitable.client import BitableClient
from robot_pm.bitable.config import BitableConfig
from robot_pm.bitable.editor import apply_edit_file
from robot_pm.bitable.errors import EditFailed, BitableError


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m robot_pm.bitable")
    subcommands = parser.add_subparsers(dest="command", required=True)
    apply_parser = subcommands.add_parser("apply", help="按编辑文件写入多维表格")
    apply_parser.add_argument("edit_file", help="编辑 JSON 的路径")
    apply_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只做校验和查询，不调用新增、更新、删除",
    )
    args = parser.parse_args(argv)
    try:
        config = BitableConfig.from_env()
        with BitableClient(config) as client:
            result = apply_edit_file(client, args.edit_file, dry_run=args.dry_run)
    except EditFailed as exc:
        print(str(exc), file=sys.stderr)
        payload = {"dry_run": False, "stopped": True, "operations": exc.completed}
        print(json.dumps(payload, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1
    except BitableError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
