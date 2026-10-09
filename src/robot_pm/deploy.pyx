# cython: language_level=3
# 部署探针。先检查主机，通过之后才创建持久化目录。失败不留下半棵树。

from libc.stdint cimport uint32_t, uint64_t

cdef extern from *:
    """
    #include <errno.h>
    #include <fcntl.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    #include <sys/file.h>
    #include <sys/ioctl.h>
    #include <sys/quota.h>
    #include <sys/stat.h>
    #include <sys/statvfs.h>
    #include <time.h>
    #include <unistd.h>

    int robot_pm_read_space(
        const char *path,
        uint64_t *bavail_bytes,
        uint64_t *bfree_bytes,
        uint64_t *frsize,
        uint64_t *favail,
        uint64_t *files) {
        struct statvfs st;
        uint64_t unit;
        if (statvfs(path, &st) != 0) {
            return errno;
        }
        unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
        if (unit == 0) {
            return EIO;
        }
        if ((uint64_t)st.f_bavail > UINT64_MAX / unit || (uint64_t)st.f_bfree > UINT64_MAX / unit) {
            return EOVERFLOW;
        }
        *frsize = unit;
        *bavail_bytes = (uint64_t)st.f_bavail * unit;
        *bfree_bytes = (uint64_t)st.f_bfree * unit;
        *favail = (uint64_t)st.f_favail;
        *files = (uint64_t)st.f_files;
        return 0;
    }

    /* Limits travel in 1024-byte quota blocks (QIF_DQBLKSIZE). curspace is bytes. */
    int robot_pm_get_quota(
        const char *special,
        int qtype,
        int id,
        uint64_t *hard_blocks,
        uint64_t *soft_blocks,
        uint64_t *cur_bytes,
        uint64_t *btime,
        uint64_t *ihard,
        uint64_t *isoft,
        uint64_t *cur_inodes,
        uint64_t *itime) {
        struct dqblk dq;
        memset(&dq, 0, sizeof(dq));
        if (quotactl(QCMD(Q_GETQUOTA, qtype), special, id, (caddr_t)&dq) != 0) {
            return errno;
        }
        *hard_blocks = dq.dqb_bhardlimit;
        *soft_blocks = dq.dqb_bsoftlimit;
        *cur_bytes = dq.dqb_curspace;
        *btime = dq.dqb_btime;
        *ihard = dq.dqb_ihardlimit;
        *isoft = dq.dqb_isoftlimit;
        *cur_inodes = dq.dqb_curinodes;
        *itime = dq.dqb_itime;
        return 0;
    }

    struct robot_pm_fsxattr {
        uint32_t fsx_xflags;
        uint32_t fsx_extsize;
        uint32_t fsx_nextents;
        uint32_t fsx_projid;
        uint32_t fsx_cowextsize;
        unsigned char fsx_pad[8];
    };

    #ifndef ROBOT_PM_FSGETXATTR
    #define ROBOT_PM_FSGETXATTR _IOR('X', 31, struct robot_pm_fsxattr)
    #endif

    int robot_pm_project_id(const char *path, uint32_t *proj_out) {
        int fd;
        struct robot_pm_fsxattr attr;
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return errno;
        }
        memset(&attr, 0, sizeof(attr));
        if (ioctl(fd, ROBOT_PM_FSGETXATTR, &attr) != 0) {
            int err = errno;
            close(fd);
            return err;
        }
        close(fd);
        *proj_out = attr.fsx_projid;
        return 0;
    }

    int robot_pm_try_lock(const char *path, int *fd_out) {
        int fd;
        char buf[64];
        int n;
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            return errno;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int err = errno;
            close(fd);
            return err;
        }
        if (fchmod(fd, 0600) != 0) {
            int err = errno;
            flock(fd, LOCK_UN);
            close(fd);
            return err;
        }
        n = snprintf(buf, sizeof(buf), "%d\\n", (int)getpid());
        if (n > 0) {
            if (ftruncate(fd, 0) == 0) {
                if (lseek(fd, 0, SEEK_SET) >= 0) {
                    ssize_t ignored = write(fd, buf, (size_t)n);
                    (void)ignored;
                }
            }
        }
        *fd_out = fd;
        return 0;
    }

    int robot_pm_unlock(int fd) {
        if (fd < 0) {
            return 0;
        }
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }

    int robot_pm_reserve(const char *path, uint64_t nbytes) {
        int fd;
        char buf[8192];
        uint64_t left;
        memset(buf, 0, sizeof(buf));
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return errno;
        }
        left = nbytes;
        while (left > 0) {
            size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            ssize_t wrote = write(fd, buf, chunk);
            if (wrote < 0) {
                int err = errno;
                if (err == EINTR) {
                    continue;
                }
                close(fd);
                unlink(path);
                return err;
            }
            if (wrote == 0) {
                close(fd);
                unlink(path);
                return ENOSPC;
            }
            left -= (uint64_t)wrote;
        }
        if (fsync(fd) != 0) {
            int err = errno;
            close(fd);
            unlink(path);
            return err;
        }
        if (close(fd) != 0) {
            unlink(path);
            return errno;
        }
        if (unlink(path) != 0) {
            return errno;
        }
        return 0;
    }

    int robot_pm_eagain(void) { return EAGAIN; }
    int robot_pm_eacces(void) { return EACCES; }
    int robot_pm_eperm(void) { return EPERM; }
    int robot_pm_erofs(void) { return EROFS; }
    int robot_pm_enospc(void) { return ENOSPC; }
    int robot_pm_edquot(void) { return EDQUOT; }
    int robot_pm_eexist(void) { return EEXIST; }
    """
    int robot_pm_read_space(
        const char *path,
        uint64_t *bavail_bytes,
        uint64_t *bfree_bytes,
        uint64_t *frsize,
        uint64_t *favail,
        uint64_t *files)
    int robot_pm_get_quota(
        const char *special,
        int qtype,
        int id,
        uint64_t *hard_blocks,
        uint64_t *soft_blocks,
        uint64_t *cur_bytes,
        uint64_t *btime,
        uint64_t *ihard,
        uint64_t *isoft,
        uint64_t *cur_inodes,
        uint64_t *itime)
    int robot_pm_project_id(const char *path, uint32_t *proj_out)
    int robot_pm_try_lock(const char *path, int *fd_out)
    int robot_pm_unlock(int fd)
    int robot_pm_reserve(const char *path, uint64_t nbytes)
    int robot_pm_eagain()
    int robot_pm_eacces()
    int robot_pm_eperm()
    int robot_pm_erofs()
    int robot_pm_enospc()
    int robot_pm_edquot()
    int robot_pm_eexist()

import os
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

# 状态计划 context.json，和 begin_plan 写入的字段一致。
INTERACTION_JSON = (
    b'{"kind":"status","item_id":"w1","seen_status":"todo","seen_start":"2026-10-01",'
    b'"seen_end":"2026-10-03","status":"doing","start":"2026-10-01","end":"2026-10-03",'
    b'"created_unix":1759276800}'
)
# 导入接受的一份工作项清单。
INBOX_JSON = (
    b'{"schema_version":1,"documents":[{"id":"doc1","type":"prd","title":"\xe8\xa7\x84\xe6\xa0\xbc",'
    b'"sections":[{"id":"sec1","title":"\xe8\x8c\x83\xe5\x9b\xb4","items":[{"id":"w1","title":"'
    b'\xe6\x8e\xa5\xe9\xa3\x9e\xe4\xb9\xa6","kind":"work","status":"todo","start":"2026-10-01",'
    b'"end":"2026-10-03","predecessors":[],"owner_role":"\xe6\x8e\xa5\xe5\x8f\xa3","source_quote":"'
    b'\xe6\x8e\xa5\xe9\xa3\x9e\xe4\xb9\xa6"}]}]}]}'
)
# 五字段墓碑。open_id / message_id 按飞书标识宽度。含换行。
TOMBSTONE_LINE = (
    b'{"interaction_id":"om_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",'
    b'"actor":"ou_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","op":"end","outcome":"cancelled",'
    b'"time":"2026-10-01 18:30:00"}\n'
)
# 30 分钟到期在删目录之前还会追加一条审计。含换行。
AUDIT_LINE = (
    b'{"time":"2026-10-01 18:30:00","op":"cancel","path":"/var/robot_pm/working/'
    b'ou_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/om_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",'
    b'"actor":"ou_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","ok":true}\n'
)

PEOPLE = 5
BLOCK_BYTES = 4096
# 文字层 PDF 或 docx 才是收件里真正留下的文件。五份同时在，样本清单的一块装不下。
INBOX_FILE_BYTES = 8 * 1024 * 1024
INBOX_ALLOWANCE_BYTES = INBOX_FILE_BYTES * PEOPLE
# events.jsonl 和 audit.jsonl 合在一起的追加余量。不随文件系统块放大。
LOG_ALLOWANCE_BYTES = 8 * 1024 * 1024
SHARED_DIRECTORIES = (
    "inbox",
    "handled",
    "working",
    "working/plans",
    "episodic",
    "semantic",
    "semantic/manifests",
    "semantic/supervision",
)
# 根目录本身也占一块。加上锁文件，骨架是 10 个 inode。
SKELETON_INODES = 1 + len(SHARED_DIRECTORIES) + 1

REQUIRED_ENV = (
    "FEISHU_APP_ID",
    "FEISHU_APP_SECRET",
    "FEISHU_ENCRYPT_KEY",
    "FEISHU_VERIFICATION_TOKEN",
    "FEISHU_BITABLE_APP_TOKEN",
    "FEISHU_BITABLE_TABLE_ID",
    "FEISHU_BOT_OPEN_ID",
)

RECORDED_FORMS = {
    "普通群": "group",
    "话题群": "topic",
}

PROBE_NAME = ".robot-pm-space-probe"
LOCK_NAME = ".writer.lock"
QUOTA_BLOCK = 1024
_Q_USR = 0
_Q_GRP = 1
_Q_PRJ = 2


class DeployError(Exception):
    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind


cdef class WriterLock:
    cdef int fd

    def __cinit__(self):
        self.fd = -1

    def __dealloc__(self):
        self.release()

    def release(self):
        if self.fd >= 0:
            robot_pm_unlock(self.fd)
            self.fd = -1

    @property
    def held(self):
        return self.fd >= 0


def interaction_bytes():
    return len(INTERACTION_JSON)


def inbox_bytes():
    return len(INBOX_JSON)


def tombstone_line_bytes():
    return len(TOMBSTONE_LINE)


def audit_line_bytes():
    return len(AUDIT_LINE)


cdef unsigned long long _alloc(unsigned long long nbytes, unsigned long long block):
    if nbytes == 0:
        return block
    return ((nbytes + block - 1) // block) * block


def _block_unit(block):
    if block < BLOCK_BYTES:
        return BLOCK_BYTES
    return block


def metadata_bytes(block=BLOCK_BYTES):
    """33 allocation units: small files, directories, the lock, and one temp file."""
    block = _block_unit(block)
    interaction = _alloc(len(INTERACTION_JSON), block)
    inbox = _alloc(len(INBOX_JSON), block)
    events = _alloc(len(TOMBSTONE_LINE) * PEOPLE, block)
    audit = _alloc(len(AUDIT_LINE) * PEOPLE, block)
    temp = events
    if audit > temp:
        temp = audit
    if interaction > temp:
        temp = interaction
    if inbox > temp:
        temp = inbox
    per_person = interaction + inbox + 2 * block
    skeleton = (1 + len(SHARED_DIRECTORIES) + 1) * block
    return int(PEOPLE * per_person + events + audit + temp + skeleton)


def persistence_minimum_bytes(block=BLOCK_BYTES):
    """Metadata blocks plus fixed inbox and log allowances."""
    return int(metadata_bytes(block) + INBOX_ALLOWANCE_BYTES + LOG_ALLOWANCE_BYTES)


def payload_bytes(block=BLOCK_BYTES):
    """Bytes still required after the skeleton directories and the lock file exist."""
    block = _block_unit(block)
    skeleton = (1 + len(SHARED_DIRECTORIES) + 1) * block
    return int(metadata_bytes(block) - skeleton + INBOX_ALLOWANCE_BYTES + LOG_ALLOWANCE_BYTES)


def inodes_needed(missing_skeleton):
    # 5 人 ×（人目录、交互目录、context、收件）+ 墓碑文件 + 审计文件 + 一个临时文件。
    payload = PEOPLE * 4 + 3
    return int(payload + missing_skeleton)


MINIMUM_BYTES = persistence_minimum_bytes(BLOCK_BYTES)
PAYLOAD_BYTES = payload_bytes(BLOCK_BYTES)
INODES_NEEDED = inodes_needed(SKELETON_INODES)


def quota_remaining_bytes(hard_blocks, soft_blocks, cur_bytes, btime, now):
    """Return remaining bytes, or None when this quota does not cap space."""
    if hard_blocks < 0 or soft_blocks < 0 or cur_bytes < 0:
        return 0
    hard = _blocks_to_bytes(hard_blocks)
    soft = _blocks_to_bytes(soft_blocks)
    return _remaining(hard, soft, cur_bytes, btime, now)


def quota_remaining_inodes(hard_inodes, soft_inodes, cur_inodes, itime, now):
    if hard_inodes < 0 or soft_inodes < 0 or cur_inodes < 0:
        return 0
    return _remaining(hard_inodes, soft_inodes, cur_inodes, itime, now)


def _blocks_to_bytes(blocks):
    if blocks <= 0:
        return 0
    if blocks > (2**64 - 1) // QUOTA_BLOCK:
        return None
    return blocks * QUOTA_BLOCK


def _remaining(hard, soft, current, grace_deadline, now):
    # None means that side is not a finite cap (zero limit, or overflow).
    cap = hard if hard else None
    if soft and current >= soft and grace_deadline and now >= grace_deadline:
        cap = soft if cap is None else min(cap, soft)
    if cap is None:
        return None
    if current >= cap:
        return 0
    return cap - current


def assess_space(
    free_bytes,
    bfree_bytes,
    quota_bytes,
    inodes_free,
    inodes_total,
    quota_inodes,
    minimum,
    inodes_required,
):
    """Decide from usable bytes. bfree_bytes is recorded and not used as the limit."""
    del bfree_bytes  # happy-path df / f_bfree must not authorize the host
    if quota_bytes is None:
        usable = free_bytes
    else:
        usable = free_bytes if free_bytes < quota_bytes else quota_bytes
    inode_limit = None
    if inodes_total > 0:
        inode_limit = inodes_free
    if quota_inodes is not None:
        inode_limit = quota_inodes if inode_limit is None else min(inode_limit, quota_inodes)
    bytes_ok = usable >= minimum
    inodes_ok = inode_limit is None or inode_limit >= inodes_required
    if bytes_ok and inodes_ok:
        return {"ok": True, "kind": "ok", "message": "", "usable": usable}
    if not bytes_ok and quota_bytes is not None and quota_bytes < minimum and free_bytes >= minimum:
        message = (
            f"配额不够：文件系统空闲有 {free_bytes} 字节，配额剩余 {quota_bytes} 字节，"
            f"至少需要 {minimum} 字节"
        )
        return {"ok": False, "kind": "quota", "message": message, "usable": usable}
    if not bytes_ok:
        message = f"空间不够：可用 {usable} 字节，持久化至少需要 {minimum} 字节"
        return {"ok": False, "kind": "space", "message": message, "usable": usable}
    stat_inodes_ok = inodes_total == 0 or inodes_free >= inodes_required
    if quota_inodes is not None and stat_inodes_ok and quota_inodes < inodes_required:
        message = (
            f"配额不够：空闲字节看起来够，配额剩余 inode {quota_inodes} 个，"
            f"至少需要 {inodes_required} 个"
        )
        return {"ok": False, "kind": "quota", "message": message, "usable": usable}
    message = (
        f"inode 不够：空闲字节看起来够，可用 inode {inode_limit} 个，"
        f"至少需要 {inodes_required} 个"
    )
    return {"ok": False, "kind": "inodes", "message": message, "usable": usable}


def missing_env(environ):
    missing = []
    for name in REQUIRED_ENV:
        value = environ.get(name, "")
        if value is None or str(value).strip() == "":
            missing.append(name)
    if not missing:
        return None
    return "缺少环境变量：" + ", ".join(missing)


def clock_problem(now, environ):
    try:
        shanghai = ZoneInfo("Asia/Shanghai")
    except ZoneInfoNotFoundError:
        return "系统没有 Asia/Shanghai 时区，不能计算北京时间的截止"
    try:
        local = datetime.fromtimestamp(now, timezone.utc).astimezone(shanghai)
    except (OverflowError, OSError, ValueError):
        return "当前时间不能换算成北京时间"
    if local.utcoffset() != timedelta(hours=8):
        return "Asia/Shanghai 的偏移不是东八区，不能计算北京时间的截止"
    floor = datetime(2026, 1, 1, tzinfo=timezone.utc).timestamp()
    ceiling = datetime(2100, 1, 1, tzinfo=timezone.utc).timestamp()
    if now < floor or now >= ceiling:
        return "当前时间不在可以计算截止的范围内"
    override = str(environ.get("ROBOT_PM_TIMEZONE", "") or "").strip()
    if override:
        try:
            ZoneInfo(override)
        except ZoneInfoNotFoundError:
            return "ROBOT_PM_TIMEZONE 不是可用时区"
    formatted = local.strftime("%Y-%m-%d %H:%M")
    if len(formatted) != 16:
        return "北京时间格式化失败"
    return None


def compiler_is_new_enough(kind, major, minor):
    if kind == "gcc":
        return major >= 14
    if kind == "clang":
        return major >= 18
    if kind == "cmake":
        return (major, minor) >= (3, 28)
    return False


def parse_tool_version(kind, text):
    if kind == "cmake":
        match = re.search(r"cmake version (\d+)\.(\d+)\.(\d+)", text)
        if not match:
            return None
        return int(match.group(1)), int(match.group(2))
    if kind == "clang":
        match = re.search(r"clang version (\d+)\.(\d+)\.(\d+)", text)
        if not match:
            return None
        return int(match.group(1)), int(match.group(2))
    first = ""
    for line in text.splitlines():
        if line.strip():
            first = line.strip()
            break
    match = re.search(r"(\d+)\.(\d+)\.(\d+)\s*$", first)
    if not match:
        return None
    return int(match.group(1)), int(match.group(2))


def toolchain_problem():
    if sys.version_info < (3, 11):
        return "Python 需要 3.11 或更高"
    if not _python3_ok():
        return "PATH 上没有 Python 3.11 或更高"
    if not _cmake_ok():
        return "没有 CMake 3.28 或更高"
    if not _cxx_ok():
        return "没有 GCC 14 或 Clang 18"
    return None


def _python3_ok():
    try:
        result = subprocess.run(
            [sys.executable, "-c", "import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)"],
            capture_output=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    if result.returncode != 0:
        return False
    try:
        which = subprocess.run(
            ["python3", "-c", "import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)"],
            capture_output=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return which.returncode == 0


def _cmake_ok():
    try:
        result = subprocess.run(
            ["cmake", "--version"],
            capture_output=True,
            timeout=10,
            check=False,
            text=True,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    if result.returncode != 0:
        return False
    parsed = parse_tool_version("cmake", result.stdout)
    if parsed is None:
        return False
    return compiler_is_new_enough("cmake", parsed[0], parsed[1])


def _cxx_ok():
    candidates = (
        ("gcc", "g++-14"),
        ("gcc", "g++-15"),
        ("clang", "clang++-18"),
        ("clang", "clang++-19"),
        ("clang", "clang++-20"),
        ("gcc", "g++"),
        ("clang", "clang++"),
    )
    for kind, binary in candidates:
        try:
            result = subprocess.run(
                [binary, "--version"],
                capture_output=True,
                timeout=10,
                check=False,
                text=True,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode != 0:
            continue
        parsed = parse_tool_version(kind, result.stdout + "\n" + result.stderr)
        if parsed is None:
            continue
        if compiler_is_new_enough(kind, parsed[0], parsed[1]):
            return True
    return False


def usable_space(path):
    """Free bytes from statvfs plus quota remaining when the filesystem reports it."""
    encoded = os.fsencode(path)
    cdef uint64_t bavail = 0
    cdef uint64_t bfree = 0
    cdef uint64_t frsize = 0
    cdef uint64_t favail = 0
    cdef uint64_t files = 0
    cdef const char *c_path = encoded
    cdef int rc = robot_pm_read_space(c_path, &bavail, &bfree, &frsize, &favail, &files)
    if rc != 0:
        raise DeployError("space", "读不到文件系统的可用空间，没有创建目录")
    quota_bytes, quota_inodes = _quota_caps(path)
    return {
        "free_bytes": int(bavail),
        "bfree_bytes": int(bfree),
        "frsize": int(frsize),
        "inodes_free": int(favail),
        "inodes_total": int(files),
        "quota_bytes": quota_bytes,
        "quota_inodes": quota_inodes,
    }


def _quota_caps(path):
    cdef uint64_t hard = 0
    cdef uint64_t soft = 0
    cdef uint64_t cur = 0
    cdef uint64_t btime = 0
    cdef uint64_t ihard = 0
    cdef uint64_t isoft = 0
    cdef uint64_t icur = 0
    cdef uint64_t itime = 0
    cdef int qrc = 0
    cdef const char *c_special = NULL
    specials = _quota_specials(path)
    if not specials:
        return None, None
    now = int(datetime.now(timezone.utc).timestamp())
    byte_caps = []
    inode_caps = []
    ids = [(_Q_USR, os.geteuid()), (_Q_GRP, os.getegid())]
    proj = _project_id(path)
    if proj is not None:
        ids.append((_Q_PRJ, proj))
    for special in specials:
        encoded = os.fsencode(special)
        c_special = encoded
        for qtype, qid in ids:
            qrc = robot_pm_get_quota(
                c_special, qtype, qid, &hard, &soft, &cur, &btime, &ihard, &isoft, &icur, &itime
            )
            if qrc != 0:
                continue
            remaining = quota_remaining_bytes(int(hard), int(soft), int(cur), int(btime), now)
            if remaining is not None:
                byte_caps.append(remaining)
            inodes = quota_remaining_inodes(int(ihard), int(isoft), int(icur), int(itime), now)
            if inodes is not None:
                inode_caps.append(inodes)
    quota_bytes = min(byte_caps) if byte_caps else None
    quota_inodes = min(inode_caps) if inode_caps else None
    return quota_bytes, quota_inodes


def _project_id(path):
    encoded = os.fsencode(path)
    cdef uint32_t proj = 0
    cdef const char *c_path = encoded
    cdef int rc = robot_pm_project_id(c_path, &proj)
    if rc != 0:
        return None
    return int(proj)


def _quota_specials(path):
    existing = path if os.path.isdir(path) else _existing_ancestor(path)
    try:
        real = os.path.realpath(existing)
    except OSError:
        return []
    found = _mount_source(real)
    specials = []
    if found is not None:
        source, mountpoint = found
        if source:
            specials.append(source)
        if mountpoint and mountpoint not in specials:
            specials.append(mountpoint)
    return specials


def _mount_source(real_path):
    try:
        text = open("/proc/self/mountinfo", "r", encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    best = None
    best_len = -1
    for line in text.splitlines():
        parts = line.split()
        if "-" not in parts:
            continue
        dash = parts.index("-")
        if dash < 5 or len(parts) < dash + 3:
            continue
        mountpoint = _unescape_mount(parts[4])
        source = _unescape_mount(parts[dash + 2])
        if real_path == mountpoint or real_path.startswith(mountpoint.rstrip("/") + "/"):
            if len(mountpoint) > best_len:
                best = (source, mountpoint)
                best_len = len(mountpoint)
    return best


def _unescape_mount(value):
    out = []
    index = 0
    while index < len(value):
        if value[index] == "\\" and index + 3 < len(value):
            digits = value[index + 1 : index + 4]
            if digits.isdigit():
                out.append(chr(int(digits, 8)))
                index += 4
                continue
        out.append(value[index])
        index += 1
    return "".join(out)


def _existing_ancestor(path):
    current = os.path.abspath(path)
    while not os.path.exists(current):
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return current


def _account_block(frsize):
    if frsize > BLOCK_BYTES:
        return frsize
    return BLOCK_BYTES


def reservation(data_root, block):
    missing = 0
    root = os.path.abspath(data_root)
    if not os.path.isdir(root):
        missing += 1 + len(SHARED_DIRECTORIES) + 1
    else:
        for relative in SHARED_DIRECTORIES:
            if not os.path.isdir(os.path.join(root, relative)):
                missing += 1
        if not os.path.exists(os.path.join(root, LOCK_NAME)):
            missing += 1
    bytes_needed = payload_bytes(block) + missing * block
    return int(bytes_needed), inodes_needed(missing)


def _fail(kind, message):
    raise DeployError(kind, message)


def _rollback(created):
    for path in reversed(created):
        try:
            if os.path.isdir(path) and not os.path.islink(path):
                for name in (LOCK_NAME, PROBE_NAME):
                    leftover = os.path.join(path, name)
                    if os.path.isfile(leftover) and not os.path.islink(leftover):
                        os.unlink(leftover)
                os.rmdir(path)
            elif os.path.lexists(path):
                os.unlink(path)
        except OSError:
            return False
    return True


def _make_dir(path, created):
    if os.path.lexists(path):
        if os.path.islink(path) or not os.path.isdir(path):
            _fail("path", f"数据目录的路径不可用：{path}")
        return
    os.mkdir(path, 0o700)
    created.append(path)
    os.chmod(path, 0o700)


def _lock(path):
    encoded = os.fsencode(path)
    cdef int fd = -1
    cdef const char *c_path = encoded
    cdef int rc = robot_pm_try_lock(c_path, &fd)
    if rc != 0:
        return rc, None
    held = WriterLock()
    held.fd = fd
    return 0, held


def _reserve(path, nbytes):
    encoded = os.fsencode(path)
    cdef const char *c_path = encoded
    return robot_pm_reserve(c_path, nbytes)


def _errno_kind(rc):
    if rc == robot_pm_eagain():
        return "lock"
    if rc in (robot_pm_eacces(), robot_pm_eperm(), robot_pm_erofs()):
        return "writable"
    if rc in (robot_pm_enospc(), robot_pm_edquot()):
        return "space"
    if rc == robot_pm_eexist():
        return "writable"
    return "space"


def _errno_message(rc, data_root, minimum):
    kind = _errno_kind(rc)
    if kind == "lock":
        return kind, "已有进程占用这棵目录的写入锁，不能再准备或共用这棵树"
    if kind == "writable":
        if rc == robot_pm_eexist():
            return kind, "探针文件已经存在，没有继续创建目录"
        return kind, f"数据目录不可写：{data_root}"
    return kind, f"空间不够：按 {minimum} 字节写入探针失败，没有创建目录"


def record_group_form(data_root, group_id, form, actual=None):
    """Write one group form row. Forms stay 普通群 or 话题群."""
    group_id = str(group_id or "").strip()
    form = str(form or "").strip()
    if not group_id or "/" in group_id or "\\" in group_id or group_id in (".", ".."):
        _fail("form", "形式不符")
    if form not in RECORDED_FORMS:
        _fail("form", "形式不符")
    if actual is not None and str(actual).strip() != RECORDED_FORMS[form]:
        _fail("form", "形式不符")
    path = os.path.join(os.path.abspath(data_root), "group-forms.jsonl")
    rows = []
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                text = line.strip()
                if not text:
                    continue
                try:
                    parsed = __import__("json").loads(text)
                except ValueError:
                    rows.append(text)
                    continue
                if isinstance(parsed, dict) and parsed.get("群标识") == group_id:
                    continue
                rows.append(text)
    import json

    rows.append(json.dumps({"群标识": group_id, "形式": form}, ensure_ascii=False, separators=(",", ":")))
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        handle.write("\n".join(rows) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def prepare(data_root, environ=None, minimum_bytes=0):
    """Probe, then create the persistence tree. The returned lock stays held."""
    if environ is None:
        environ = os.environ
    env_message = missing_env(environ)
    if env_message:
        _fail("env", env_message)
    clock_message = clock_problem(datetime.now(timezone.utc).timestamp(), environ)
    if clock_message:
        _fail("clock", clock_message)
    tool_message = toolchain_problem()
    if tool_message:
        _fail("toolchain", tool_message)

    root = os.path.abspath(data_root)
    if os.path.islink(root):
        _fail("path", f"数据目录的路径不可用：{root}")
    if os.path.exists(root) and not os.path.isdir(root):
        _fail("path", f"数据目录的路径不可用：{root}")
    ancestor = root if os.path.isdir(root) else _existing_ancestor(root)
    if not os.path.isdir(ancestor):
        _fail("path", f"数据目录的路径不可用：{root}")

    space = usable_space(ancestor if not os.path.isdir(root) else root)
    block = _account_block(space["frsize"])
    needed_bytes, needed_inodes = reservation(root, block)
    if minimum_bytes > 0:
        needed_bytes = int(minimum_bytes)
    decision = assess_space(
        space["free_bytes"],
        space["bfree_bytes"],
        space["quota_bytes"],
        space["inodes_free"],
        space["inodes_total"],
        space["quota_inodes"],
        needed_bytes,
        needed_inodes,
    )
    if not decision["ok"]:
        _fail(decision["kind"], decision["message"])

    created = []
    lock = None
    probe = None
    try:
        if os.path.isdir(root):
            rc, lock = _lock(os.path.join(root, LOCK_NAME))
            if rc != 0:
                kind, message = _errno_message(rc, root, needed_bytes)
                _fail(kind, message)
            probe = os.path.join(root, PROBE_NAME)
            rc = _reserve(probe, needed_bytes)
            if rc != 0:
                kind, message = _errno_message(rc, root, needed_bytes)
                _fail(kind, message)
            probe = None
        else:
            probe_dir = ancestor
            probe = os.path.join(probe_dir, PROBE_NAME)
            rc = _reserve(probe, needed_bytes)
            if rc != 0:
                kind, message = _errno_message(rc, root, needed_bytes)
                _fail(kind, message)
            probe = None
            _make_missing_parents(root, created)
            _make_dir(root, created)
            rc, lock = _lock(os.path.join(root, LOCK_NAME))
            if rc != 0:
                kind, message = _errno_message(rc, root, needed_bytes)
                if kind == "lock":
                    # 另一进程已经拿到锁。不删除它正在用的目录。
                    created.clear()
                _fail(kind, message)
        for relative in SHARED_DIRECTORIES:
            _make_dir(os.path.join(root, relative), created)
        if os.stat(root).st_uid == os.geteuid():
            os.chmod(root, 0o700)
        again = usable_space(root)
        again_need, again_inodes = reservation(root, _account_block(again["frsize"]))
        if minimum_bytes <= 0:
            again_decision = assess_space(
                again["free_bytes"],
                again["bfree_bytes"],
                again["quota_bytes"],
                again["inodes_free"],
                again["inodes_total"],
                again["quota_inodes"],
                again_need,
                again_inodes,
            )
            if not again_decision["ok"]:
                _fail(again_decision["kind"], again_decision["message"])
        return lock
    except DeployError:
        if lock is not None:
            lock.release()
        if not _rollback(created):
            raise DeployError("space", "准备失败，目录没有回到原状")
        raise
    except OSError as exc:
        if lock is not None:
            lock.release()
        _rollback(created)
        raise DeployError("writable", f"数据目录不可写：{root}") from exc


def _make_missing_parents(root, created):
    root = os.path.abspath(root)
    missing = []
    current = os.path.dirname(root)
    while current and not os.path.isdir(current):
        missing.append(current)
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    for path in reversed(missing):
        _make_dir(path, created)


def main(argv=None):
    import argparse

    parser = argparse.ArgumentParser(
        prog="robot-pm-deploy",
        description="只准备目录，不启动程序。",
        add_help=False,
    )
    parser.add_argument("-h", "--帮助", action="help", help="显示这些说明")
    parser.add_argument(
        "--目录",
        dest="data_root",
        default=None,
        metavar="路径",
        help="数据放在这个目录",
    )
    parser.add_argument(
        "--占着",
        dest="hold",
        action="store_true",
        help="准备好后先占着，不让别人同时准备",
    )
    parser.add_argument("--群", dest="group_id", default=None, metavar="标识", help="记下这个群")
    parser.add_argument("--形式", dest="form", default=None, metavar="形式", help="普通群或话题群")
    parser.add_argument("--实际", dest="actual", default=None, metavar="形式", help="录入时核对的实际形式")
    args = parser.parse_args(argv)
    if (args.group_id is None) != (args.form is None):
        print("形式不符", file=sys.stderr)
        return 1
    if args.data_root:
        data_root = args.data_root
    else:
        data_root = os.environ.get("ROBOT_PM_DATA_ROOT", "").strip() or "var/robot_pm"
    try:
        lock = prepare(data_root)
        if args.group_id is not None:
            record_group_form(data_root, args.group_id, args.form, args.actual)
    except DeployError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(f"数据目录已就绪：{os.path.abspath(data_root)}", flush=True)
    print(f"至少需要 {MINIMUM_BYTES} 字节", flush=True)
    try:
        if args.hold:
            sys.stdin.read()
    finally:
        lock.release()
    return 0
