"""Command line for the Bitable editor.

    python -m robot_pm.bitable 写入 path/to/edit.json --只检查
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
    parser = argparse.ArgumentParser(
        prog="python -m robot_pm.bitable",
        description="按文件改表格。",
        add_help=False,
    )
    parser.add_argument("-h", "--帮助", action="help", help="显示这些说明")
    subcommands = parser.add_subparsers(dest="command", required=True)
    apply_parser = subcommands.add_parser("写入", help="按文件改表格", add_help=False)
    apply_parser.add_argument("-h", "--帮助", action="help", help="显示这些说明")
    apply_parser.add_argument("edit_file", metavar="文件", help="要改的文件")
    apply_parser.add_argument(
        "--只检查",
        dest="dry_run",
        action="store_true",
        help="只检查，不改表格",
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
