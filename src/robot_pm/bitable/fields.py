"""Field metadata and the write-value shapes verified against Feishu docs."""

from __future__ import annotations

import math
import re
from dataclasses import dataclass

from robot_pm.bitable.errors import BitableAPIError, FieldError

# ui_type -> (bitable type number, value kind).
# Write shapes follow the create/update record examples, not the read shapes
# in the data-structure overview.
SUPPORTED: dict[str, tuple[int, str]] = {
    "Text": (1, "string"),
    "Barcode": (1, "string"),
    "Email": (1, "string"),
    "Number": (2, "number"),
    "Progress": (2, "number"),
    "Currency": (2, "number"),
    "Rating": (2, "number"),
    "SingleSelect": (3, "string"),
    "MultiSelect": (4, "string_list"),
    "DateTime": (5, "millis"),
    "Checkbox": (7, "bool"),
    "User": (11, "id_list"),
    "Phone": (13, "phone"),
    "Url": (15, "url"),
    "Attachment": (17, "file_token_list"),
    "SingleLink": (18, "record_id_list"),
    "DuplexLink": (21, "record_id_list"),
    "Location": (22, "location"),
    "GroupChat": (23, "id_list"),
}

# `is` filters are documented with a string array. These ui types have a
# single scalar write value that converts to one filter string.
KEY_UI_TYPES = frozenset(
    {
        "Text",
        "Barcode",
        "Email",
        "Number",
        "Progress",
        "Currency",
        "Rating",
        "SingleSelect",
        "Phone",
    }
)

_DEFAULT_UI_BY_TYPE = {
    1: "Text",
    2: "Number",
    3: "SingleSelect",
    4: "MultiSelect",
    5: "DateTime",
    7: "Checkbox",
    11: "User",
    13: "Phone",
    15: "Url",
    17: "Attachment",
    18: "SingleLink",
    21: "DuplexLink",
    22: "Location",
    23: "GroupChat",
}

_PHONE_RE = re.compile(r"(\+)?\d*")
_LOCATION_RE = re.compile(r"-?\d+(\.\d+)?,-?\d+(\.\d+)?")


@dataclass(frozen=True)
class FieldMeta:
    name: str
    type_code: int
    ui_type: str

    @property
    def kind(self) -> str | None:
        spec = SUPPORTED.get(self.ui_type)
        if spec is None or spec[0] != self.type_code:
            return None
        return spec[1]


class FieldCatalog:
    def __init__(self, fields: dict[str, FieldMeta]) -> None:
        self.by_name = fields

    @classmethod
    def from_items(cls, items: list[dict]) -> FieldCatalog:
        found: dict[str, FieldMeta] = {}
        for item in items:
            name = item.get("field_name")
            type_code = item.get("type")
            if not isinstance(name, str) or not name:
                raise BitableAPIError("列出字段的返回缺少 field_name")
            if isinstance(type_code, bool) or not isinstance(type_code, int):
                raise BitableAPIError(f"字段 {name} 缺少整数 type")
            if name in found:
                raise BitableAPIError(f"列出字段返回了重复的字段名：{name}")
            ui_type = item.get("ui_type")
            if not isinstance(ui_type, str) or not ui_type:
                ui_type = _DEFAULT_UI_BY_TYPE.get(type_code, "")
            found[name] = FieldMeta(name=name, type_code=type_code, ui_type=ui_type)
        return cls(found)

    def require_writable(self, name: str) -> FieldMeta:
        meta = self.by_name.get(name)
        if meta is None:
            known = "、".join(sorted(self.by_name)) or "（表中没有字段）"
            raise FieldError(f"未知字段：{name}。当前表的字段名为：{known}")
        if meta.kind is None:
            raise FieldError(
                f"不支持的字段类型：{name}（type={meta.type_code}, ui_type={meta.ui_type or '空'}）。"
                "本编辑程序只写入已核对过写入格式的类型。"
            )
        return meta

    def check_record(self, fields: dict) -> None:
        if not isinstance(fields, dict) or not fields:
            raise FieldError("fields 必须是非空对象。")
        for name, value in fields.items():
            if not isinstance(name, str) or not name:
                raise FieldError("字段名必须是非空字符串。")
            meta = self.require_writable(name)
            if value is None:
                continue
            _check_value(meta, value)

    def check_key(self, name: str, value: object) -> str:
        meta = self.require_writable(name)
        if meta.ui_type not in KEY_UI_TYPES:
            raise FieldError(
                f"字段 {name} 的类型 {meta.ui_type} 不能当作 upsert 键。"
                "键字段只支持文本、条码、邮箱、数字、进度、货币、评分、单选、电话号码。"
            )
        if value is None or value == "":
            raise FieldError(f"upsert 键字段 {name} 不能为空。")
        _check_value(meta, value)
        return filter_literal(value)


def filter_literal(value: object) -> str:
    """Turn a key value into the string Feishu's `is` filter expects."""
    if isinstance(value, str):
        return value
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise FieldError("upsert 键的值必须是字符串或数字。")
    if isinstance(value, float):
        if not math.isfinite(value):
            raise FieldError("upsert 键的数字必须是有限值。")
        text = format(value, "f").rstrip("0").rstrip(".")
        return text or "0"
    return str(value)


def _check_value(meta: FieldMeta, value: object) -> None:
    kind = meta.kind
    name = meta.name
    if kind == "string":
        if not isinstance(value, str):
            raise FieldError(f"字段 {name} 需要字符串。")
        return
    if kind == "number":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise FieldError(f"字段 {name} 需要数字。")
        if isinstance(value, float) and not math.isfinite(value):
            raise FieldError(f"字段 {name} 的数字必须是有限值。")
        return
    if kind == "millis":
        if isinstance(value, bool) or not isinstance(value, int):
            raise FieldError(f"字段 {name} 需要毫秒级 Unix 时间戳（整数）。")
        return
    if kind == "bool":
        if not isinstance(value, bool):
            raise FieldError(f"字段 {name} 需要 true 或 false。")
        return
    if kind == "phone":
        if not isinstance(value, str) or len(value) > 64 or _PHONE_RE.fullmatch(value) is None:
            raise FieldError(
                f"字段 {name} 需要电话字符串：可选前缀 +，其余为数字，最长 64。"
            )
        return
    if kind == "location":
        if not isinstance(value, str) or _LOCATION_RE.fullmatch(value) is None:
            raise FieldError(
                f"字段 {name} 需要写入用的经纬度字符串，例如 \"116.397755,39.903179\"。"
            )
        return
    if kind == "string_list" or kind == "record_id_list":
        if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
            raise FieldError(f"字段 {name} 需要由非空字符串组成的数组。")
        return
    if kind == "id_list":
        if not isinstance(value, list) or not all(_is_id_object(item) for item in value):
            raise FieldError(
                f"字段 {name} 需要只含 id 的对象数组，例如 [{{\"id\": \"ou_example\"}}]。"
            )
        return
    if kind == "url":
        if (
            not isinstance(value, dict)
            or set(value) != {"text", "link"}
            or not isinstance(value["text"], str)
            or not isinstance(value["link"], str)
            or not value["text"]
            or not value["link"]
        ):
            raise FieldError(f"字段 {name} 需要对象 {{\"text\": \"...\", \"link\": \"...\"}}。")
        return
    if kind == "file_token_list":
        if not isinstance(value, list) or not all(_is_file_token(item) for item in value):
            raise FieldError(
                f"字段 {name} 需要只含 file_token 的对象数组，例如 [{{\"file_token\": \"box_example\"}}]。"
            )
        return
    raise FieldError(f"不支持的字段类型：{name}（ui_type={meta.ui_type}）。")


def _is_id_object(value: object) -> bool:
    return (
        isinstance(value, dict)
        and set(value) == {"id"}
        and isinstance(value["id"], str)
        and bool(value["id"])
    )


def _is_file_token(value: object) -> bool:
    return (
        isinstance(value, dict)
        and set(value) == {"file_token"}
        and isinstance(value["file_token"], str)
        and bool(value["file_token"])
    )
