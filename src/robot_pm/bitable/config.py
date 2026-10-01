"""Runtime configuration. Values come from the environment, never from the repo."""

from __future__ import annotations

import os
from dataclasses import dataclass

from robot_pm.bitable.errors import ConfigError

DEFAULT_BASE_URL = "https://open.feishu.cn"

_REQUIRED = (
    "FEISHU_APP_ID",
    "FEISHU_APP_SECRET",
    "FEISHU_BITABLE_APP_TOKEN",
    "FEISHU_BITABLE_TABLE_ID",
)


@dataclass(frozen=True)
class BitableConfig:
    app_id: str
    app_secret: str
    app_token: str
    table_id: str
    base_url: str = DEFAULT_BASE_URL

    @classmethod
    def from_env(cls, env: dict[str, str] | None = None) -> BitableConfig:
        source = os.environ if env is None else env
        missing = [name for name in _REQUIRED if not source.get(name, "").strip()]
        if missing:
            names = ", ".join(missing)
            raise ConfigError(f"缺少环境变量：{names}")
        base_url = source.get("FEISHU_BASE_URL", "").strip() or DEFAULT_BASE_URL
        return cls(
            app_id=source["FEISHU_APP_ID"].strip(),
            app_secret=source["FEISHU_APP_SECRET"].strip(),
            app_token=source["FEISHU_BITABLE_APP_TOKEN"].strip(),
            table_id=source["FEISHU_BITABLE_TABLE_ID"].strip(),
            base_url=base_url.rstrip("/"),
        )

    def secret_values(self) -> tuple[str, ...]:
        return (self.app_secret, self.app_token, self.app_id)
