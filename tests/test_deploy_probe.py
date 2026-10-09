"""Probe tests. Placeholders only; nothing here is a Feishu credential."""

import os
import stat
import subprocess
import sys
import threading
from datetime import datetime, timezone

import pytest

from robot_pm.deploy import (
    BLOCK_BYTES,
    INBOX_ALLOWANCE_BYTES,
    INBOX_FILE_BYTES,
    INODES_NEEDED,
    LOG_ALLOWANCE_BYTES,
    MINIMUM_BYTES,
    PAYLOAD_BYTES,
    REQUIRED_ENV,
    DeployError,
    assess_space,
    audit_line_bytes,
    clock_problem,
    compiler_is_new_enough,
    inbox_bytes,
    interaction_bytes,
    metadata_bytes,
    missing_env,
    parse_tool_version,
    persistence_minimum_bytes,
    prepare,
    quota_remaining_bytes,
    tombstone_line_bytes,
    usable_space,
)

SECRET = "sentinel-secret-do-not-print"


def probe_env(**overrides):
    env = {
        "PATH": os.environ.get("PATH", ""),
        "HOME": os.environ.get("HOME", ""),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "FEISHU_APP_ID": "cli_example_app_id",
        "FEISHU_APP_SECRET": SECRET,
        "FEISHU_ENCRYPT_KEY": "example_encrypt_key",
        "FEISHU_VERIFICATION_TOKEN": "example_verification_token",
        "FEISHU_BITABLE_APP_TOKEN": "example_bitable_app_token",
        "FEISHU_BITABLE_TABLE_ID": "tbl_example",
        "FEISHU_BOT_OPEN_ID": "ou_bot",
    }
    env.update(overrides)
    return env


def test_minimum_bytes_cover_inbox_files_and_logs():
    assert interaction_bytes() == 186
    assert inbox_bytes() == 301
    assert tombstone_line_bytes() == 165
    assert audit_line_bytes() == 204
    assert metadata_bytes(BLOCK_BYTES) == 135168
    assert metadata_bytes(BLOCK_BYTES) == 33 * BLOCK_BYTES
    assert INBOX_FILE_BYTES == 8388608
    assert INBOX_ALLOWANCE_BYTES == 8388608 * 5
    assert INBOX_ALLOWANCE_BYTES == 41943040
    assert LOG_ALLOWANCE_BYTES == 8388608
    assert persistence_minimum_bytes(BLOCK_BYTES) == 135168 + 41943040 + 8388608
    assert MINIMUM_BYTES == 50466816
    assert PAYLOAD_BYTES == 50466816 - 10 * BLOCK_BYTES
    assert INODES_NEEDED == 33
    assert persistence_minimum_bytes(8192) == 33 * 8192 + 41943040 + 8388608
    assert metadata_bytes(8192) == 33 * 8192
    assert persistence_minimum_bytes(1024) == 50466816


def test_quota_block_is_1024_bytes_and_zero_limit_is_not_a_cap():
    assert quota_remaining_bytes(100, 0, 0, 0, 0) == 100 * 1024
    assert quota_remaining_bytes(0, 0, 999999, 0, 0) is None
    assert quota_remaining_bytes(0, 10, 10 * 1024, 50, 50) == 0


def test_free_bytes_that_only_look_reserved_or_quotad_fail():
    looks_free = assess_space(
        free_bytes=10,
        bfree_bytes=10**15,
        quota_bytes=None,
        inodes_free=1000,
        inodes_total=1000,
        quota_inodes=None,
        minimum=MINIMUM_BYTES,
        inodes_required=INODES_NEEDED,
    )
    assert looks_free["ok"] is False
    assert looks_free["kind"] == "space"
    assert "10" in looks_free["message"]
    assert str(10**15) not in looks_free["message"]

    looks_ok = assess_space(
        free_bytes=10**15,
        bfree_bytes=10**15,
        quota_bytes=100,
        inodes_free=1000,
        inodes_total=1000,
        quota_inodes=None,
        minimum=MINIMUM_BYTES,
        inodes_required=INODES_NEEDED,
    )
    assert looks_ok["ok"] is False
    assert looks_ok["kind"] == "quota"

    inodes = assess_space(
        free_bytes=10**15,
        bfree_bytes=10**15,
        quota_bytes=None,
        inodes_free=1,
        inodes_total=100,
        quota_inodes=None,
        minimum=MINIMUM_BYTES,
        inodes_required=INODES_NEEDED,
    )
    assert inodes["kind"] == "inodes"


def test_compiler_versions_reject_gcc_13_and_accept_gcc_14():
    gcc13 = "g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\nCopyright\n"
    gcc14 = "g++-14 (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0\n"
    clang = "Ubuntu clang version 18.1.3 (1ubuntu1)\n"
    parsed13 = parse_tool_version("gcc", gcc13)
    parsed14 = parse_tool_version("gcc", gcc14)
    parsed_clang = parse_tool_version("clang", clang)
    parsed_cmake = parse_tool_version("cmake", "cmake version 3.27.0\n")
    assert parsed13 == (13, 3)
    assert compiler_is_new_enough("gcc", *parsed13) is False
    assert compiler_is_new_enough("gcc", *parsed14) is True
    assert compiler_is_new_enough("clang", *parsed_clang) is True
    assert compiler_is_new_enough("cmake", *parsed_cmake) is False
    assert compiler_is_new_enough("cmake", 3, 28) is True


def test_clock_rejects_epoch_and_unknown_zone_without_printing_it():
    assert clock_problem(0, {}) is not None
    message = clock_problem(datetime.now(timezone.utc).timestamp(), {"ROBOT_PM_TIMEZONE": "Not/AZone"})
    assert message is not None
    assert "Not/AZone" not in message
    assert clock_problem(datetime.now(timezone.utc).timestamp(), {}) is None


def test_missing_env_stops_before_creating_a_tree(tmp_path):
    env = probe_env()
    env.pop("FEISHU_APP_SECRET")
    root = tmp_path / "var" / "robot_pm"
    with pytest.raises(DeployError) as caught:
        prepare(root, environ=env)
    assert caught.value.kind == "env"
    assert "FEISHU_APP_SECRET" in str(caught.value)
    assert SECRET not in str(caught.value)
    assert not root.exists()


def test_too_little_space_does_not_create_the_tree(tmp_path):
    root = tmp_path / "robot_pm"
    info = usable_space(tmp_path)
    with pytest.raises(DeployError) as caught:
        prepare(root, environ=probe_env(), minimum_bytes=info["free_bytes"] + 1)
    assert caught.value.kind == "space"
    assert SECRET not in str(caught.value)
    assert not root.exists()
    assert not (tmp_path / ".robot-pm-space-probe").exists()


def test_not_writable_does_not_create_children(tmp_path):
    root = tmp_path / "robot_pm"
    root.mkdir()
    root.chmod(0o555)
    try:
        with pytest.raises(DeployError) as caught:
            prepare(root, environ=probe_env())
        assert caught.value.kind == "writable"
        assert "不可写" in str(caught.value)
        assert SECRET not in str(caught.value)
        assert not (root / "inbox").exists()
        assert not (root / ".writer.lock").exists()
    finally:
        root.chmod(0o755)


def test_prepare_creates_the_tree_and_a_second_process_cannot_share_it(tmp_path):
    root = tmp_path / "robot_pm"
    env = probe_env()
    code = (
        "import sys\n"
        "from robot_pm.deploy import main\n"
        "sys.exit(main(['--目录', sys.argv[1], '--占着']))\n"
    )
    first = subprocess.Popen(
        [sys.executable, "-c", code, str(root)],
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    ready = []

    def read_ready():
        ready.append(first.stdout.readline())

    reader = threading.Thread(target=read_ready)
    reader.start()
    reader.join(60)
    assert reader.is_alive() is False
    assert first.poll() is None
    assert "数据目录已就绪" in (ready[0] if ready else "")
    assert SECRET not in (ready[0] if ready else "")
    for relative in (
        "inbox",
        "handled",
        "working/plans",
        "episodic",
        "semantic/manifests",
        "semantic/supervision",
    ):
        assert (root / relative).is_dir()
    assert stat.S_IMODE(root.stat().st_mode) == 0o700
    before = sorted(path.name for path in root.iterdir())
    second = subprocess.run(
        [sys.executable, "-c", "import sys\nfrom robot_pm.deploy import main\nsys.exit(main(['--目录', sys.argv[1]]))", str(root)],
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert second.returncode == 1
    assert "写入锁" in second.stderr
    assert SECRET not in second.stderr + second.stdout
    assert sorted(path.name for path in root.iterdir()) == before
    first.stdin.close()
    assert first.wait(timeout=30) == 0
    err = first.stderr.read()
    assert SECRET not in err
    again = prepare(root, environ=env)
    again.release()


def test_record_group_form_writes_one_row_and_rejects_a_mismatch(tmp_path):
    from robot_pm.deploy import record_group_form

    root = tmp_path / "robot_pm"
    root.mkdir()
    record_group_form(root, "oc_group", "普通群", actual="group")
    text = (root / "group-forms.jsonl").read_text(encoding="utf-8")
    assert '"群标识":"oc_group"' in text
    assert '"形式":"普通群"' in text
    with pytest.raises(DeployError) as caught:
        record_group_form(root, "oc_group", "普通群", actual="topic")
    assert caught.value.kind == "form"
    with pytest.raises(DeployError):
        record_group_form(root, "oc_group", "别的群")


def test_usable_space_reports_bavail_separately_from_bfree(tmp_path):
    info = usable_space(tmp_path)
    assert info["free_bytes"] <= info["bfree_bytes"]
    assert info["frsize"] > 0
    assert "quota_bytes" in info


def test_required_names_are_the_placeholders_from_the_example():
    assert missing_env(probe_env()) is None
    assert "FEISHU_APP_SECRET" in REQUIRED_ENV
