"""Apply an edit file to one Bitable table.

The adapter in ``client`` performs HTTP. This module parses the edit file,
checks field names and value shapes, and calls the adapter. Dry-run does
the same reads and checks, then skips every write.
"""

from __future__ import annotations

import json
from pathlib import Path

from robot_pm.bitable.client import BitableClient
from robot_pm.bitable.errors import EditFailed, FieldError, PartialBatchError
from robot_pm.bitable.fields import FieldCatalog

_CREATE_FORMS = ({"op", "fields"}, {"op", "records"})
_UPDATE_FORMS = ({"op", "record_id", "fields"}, {"op", "records"})
_DELETE_FORMS = ({"op", "record_id"}, {"op", "record_ids"})
_UPSERT_FORMS = ({"op", "key_field", "fields"}, {"op", "key_field", "records"})


def apply_edit_file(client: BitableClient, path: str | Path, *, dry_run: bool) -> dict:
    edit_path = Path(path)
    try:
        raw = edit_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise FieldError(f"找不到编辑文件：{edit_path}") from exc
    try:
        document = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise FieldError(f"编辑文件不是合法 JSON：{exc.msg}") from exc
    return apply_edit(client, document, dry_run=dry_run)


def apply_edit(client: BitableClient, document: object, *, dry_run: bool) -> dict:
    operations = _parse_document(document)
    catalog = client.field_catalog()
    _validate(catalog, operations)
    completed: list[dict] = []
    # Keys upserted earlier in this run. Plain creates are not indexed.
    local_keys: dict[tuple[str, str], str | None] = {}
    for index, operation in enumerate(operations):
        try:
            result = _apply_operation(
                client,
                catalog,
                operation,
                dry_run=dry_run,
                local_keys=local_keys,
            )
        except PartialBatchError as exc:
            completed.append(
                {
                    "index": index,
                    "op": operation["op"],
                    "status": "partial",
                    "record_ids": exc.applied,
                    "not_applied": exc.rejected,
                }
            )
            raise EditFailed(
                f"第 {index + 1} 个操作（{operation['op']}）部分失败：{exc} "
                "本操作之后的编辑已停止，此前成功的写入不会回滚。",
                completed=completed,
            ) from exc
        except EditFailed:
            raise
        except Exception as exc:
            raise EditFailed(
                f"第 {index + 1} 个操作（{operation['op']}）失败：{exc} "
                "本操作之后的编辑已停止，此前成功的写入不会回滚。",
                completed=completed,
            ) from exc
        result["index"] = index
        result["status"] = "planned" if dry_run else "applied"
        completed.append(result)
    return {"dry_run": dry_run, "operations": completed}


def _apply_operation(
    client: BitableClient,
    catalog: FieldCatalog,
    operation: dict,
    *,
    dry_run: bool,
    local_keys: dict[tuple[str, str], str | None],
) -> dict:
    kind = operation["op"]
    if kind == "create":
        return _apply_create(client, operation, dry_run=dry_run)
    if kind == "update":
        return _apply_update(client, operation, dry_run=dry_run)
    if kind == "delete":
        return _apply_delete(client, operation, dry_run=dry_run)
    if kind == "upsert":
        return _apply_upsert(
            client,
            catalog,
            operation,
            dry_run=dry_run,
            local_keys=local_keys,
        )
    raise FieldError(f"未知操作：{kind}")


def _apply_create(client: BitableClient, operation: dict, *, dry_run: bool) -> dict:
    records = _record_fields(operation)
    if dry_run:
        return {"op": "create", "action": "create", "count": len(records), "record_ids": []}
    if len(records) == 1:
        record_ids = [client.create_record(records[0])]
    else:
        record_ids = client.batch_create_records(records)
    return {"op": "create", "action": "create", "count": len(record_ids), "record_ids": record_ids}


def _apply_update(client: BitableClient, operation: dict, *, dry_run: bool) -> dict:
    records = _update_records(operation)
    record_ids = [record["record_id"] for record in records]
    if dry_run:
        return {"op": "update", "action": "update", "count": len(records), "record_ids": record_ids}
    if len(records) == 1:
        client.update_record(records[0]["record_id"], records[0]["fields"])
    else:
        client.batch_update_records(records)
    return {"op": "update", "action": "update", "count": len(record_ids), "record_ids": record_ids}


def _apply_delete(client: BitableClient, operation: dict, *, dry_run: bool) -> dict:
    record_ids = _delete_ids(operation)
    if dry_run:
        return {"op": "delete", "action": "delete", "count": len(record_ids), "record_ids": record_ids}
    if len(record_ids) == 1:
        deleted = [client.delete_record(record_ids[0])]
    else:
        deleted = client.batch_delete_records(record_ids)
    return {"op": "delete", "action": "delete", "count": len(deleted), "record_ids": deleted}


def _apply_upsert(
    client: BitableClient,
    catalog: FieldCatalog,
    operation: dict,
    *,
    dry_run: bool,
    local_keys: dict[tuple[str, str], str | None],
) -> dict:
    key_field = operation["key_field"]
    rows = []
    pending_creates: set[str] = set()
    for fields in _record_fields(operation):
        if key_field not in fields:
            raise FieldError(f"upsert 的每条记录都必须包含键字段 {key_field}")
        literal = catalog.check_key(key_field, fields[key_field])
        marker = (key_field, literal)
        known = local_keys.get(marker)
        if known:
            rows.append(
                {
                    "action": "update",
                    "key_field": key_field,
                    "key": literal,
                    "record_id": known,
                    "fields": fields,
                    "depends_on_create": False,
                }
            )
            continue
        if marker in local_keys or literal in pending_creates:
            rows.append(
                {
                    "action": "update",
                    "key_field": key_field,
                    "key": literal,
                    "record_id": None,
                    "fields": fields,
                    "depends_on_create": True,
                }
            )
            continue
        found = client.search_record_ids(key_field, literal)
        if len(found) > 1:
            raise FieldError(
                f"键字段 {key_field} 的值 {literal} 匹配到 {len(found)} 条记录，"
                "无法幂等更新。本次 upsert 不会写入。"
            )
        if len(found) == 1:
            local_keys[marker] = found[0]
            rows.append(
                {
                    "action": "update",
                    "key_field": key_field,
                    "key": literal,
                    "record_id": found[0],
                    "fields": fields,
                    "depends_on_create": False,
                }
            )
            continue
        pending_creates.add(literal)
        local_keys[marker] = None
        rows.append(
            {
                "action": "create",
                "key_field": key_field,
                "key": literal,
                "record_id": None,
                "fields": fields,
                "depends_on_create": False,
            }
        )
    if dry_run:
        return {"op": "upsert", "rows": [_public_row(row) for row in rows]}

    existing_updates = [
        row for row in rows if row["action"] == "update" and not row["depends_on_create"]
    ]
    creates = [row for row in rows if row["action"] == "create"]
    follow_updates = [row for row in rows if row["depends_on_create"]]
    _write_updates(client, existing_updates)
    _write_creates(client, key_field, creates, local_keys)
    for row in follow_updates:
        row["record_id"] = local_keys[(key_field, row["key"])]
        if not row["record_id"]:
            raise FieldError(f"键字段 {key_field} 的值 {row['key']} 没有拿到新建记录的 record_id")
    _write_updates(client, follow_updates)
    return {"op": "upsert", "rows": [_public_row(row) for row in rows]}


def _write_updates(client: BitableClient, rows: list[dict]) -> None:
    if not rows:
        return
    payload = [{"record_id": row["record_id"], "fields": row["fields"]} for row in rows]
    if len(payload) == 1:
        client.update_record(payload[0]["record_id"], payload[0]["fields"])
        return
    client.batch_update_records(payload)


def _write_creates(
    client: BitableClient,
    key_field: str,
    rows: list[dict],
    local_keys: dict[tuple[str, str], str | None],
) -> None:
    if not rows:
        return
    fields = [row["fields"] for row in rows]
    if len(fields) == 1:
        record_ids = [client.create_record(fields[0])]
    else:
        record_ids = client.batch_create_records(fields)
    for row, record_id in zip(rows, record_ids):
        row["record_id"] = record_id
        local_keys[(key_field, row["key"])] = record_id


def _public_row(row: dict) -> dict:
    return {
        "action": row["action"],
        "key_field": row["key_field"],
        "key": row["key"],
        "record_id": row["record_id"],
        "depends_on_create": row["depends_on_create"],
    }


def _validate(catalog: FieldCatalog, operations: list[dict]) -> None:
    for index, operation in enumerate(operations):
        try:
            kind = operation["op"]
            if kind == "create":
                for fields in _record_fields(operation):
                    catalog.check_record(fields)
            elif kind == "update":
                for record in _update_records(operation):
                    catalog.check_record(record["fields"])
            elif kind == "delete":
                _delete_ids(operation)
            elif kind == "upsert":
                key_field = operation["key_field"]
                seen: set[str] = set()
                for fields in _record_fields(operation):
                    if key_field not in fields:
                        raise FieldError(f"upsert 的每条记录都必须包含键字段 {key_field}")
                    literal = catalog.check_key(key_field, fields[key_field])
                    catalog.check_record(fields)
                    if literal in seen:
                        continue
                    seen.add(literal)
            else:
                raise FieldError(f"未知操作：{kind}")
        except FieldError as exc:
            raise FieldError(f"第 {index + 1} 个操作（{operation.get('op')}）不合法：{exc}") from exc


def _parse_document(document: object) -> list[dict]:
    if not isinstance(document, dict) or set(document) != {"operations"}:
        raise FieldError("编辑文件的顶层只能有 operations。")
    operations = document["operations"]
    if not isinstance(operations, list) or not operations:
        raise FieldError("operations 必须是非空数组。")
    parsed: list[dict] = []
    for index, operation in enumerate(operations):
        if not isinstance(operation, dict):
            raise FieldError(f"第 {index + 1} 个操作必须是对象。")
        kind = operation.get("op")
        if kind == "create":
            parsed.append(_parse_create(operation, index))
        elif kind == "update":
            parsed.append(_parse_update(operation, index))
        elif kind == "delete":
            parsed.append(_parse_delete(operation, index))
        elif kind == "upsert":
            parsed.append(_parse_upsert(operation, index))
        else:
            raise FieldError(f"第 {index + 1} 个操作的 op 必须是 create、update、delete 或 upsert。")
    return parsed


def _parse_create(operation: dict, index: int) -> dict:
    _require_keys(operation, _CREATE_FORMS, index)
    if "fields" in operation:
        return {"op": "create", "records": [{"fields": _fields_object(operation["fields"], index)}]}
    return {"op": "create", "records": _field_records(operation["records"], index)}


def _parse_update(operation: dict, index: int) -> dict:
    _require_keys(operation, _UPDATE_FORMS, index)
    if "record_id" in operation:
        return {
            "op": "update",
            "records": [
                {
                    "record_id": _record_id(operation["record_id"], index),
                    "fields": _fields_object(operation["fields"], index),
                }
            ],
        }
    records = operation["records"]
    if not isinstance(records, list) or not records:
        raise FieldError(f"第 {index + 1} 个操作的 records 必须是非空数组。")
    parsed = []
    seen: set[str] = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"record_id", "fields"}:
            raise FieldError(
                f"第 {index + 1} 个操作的更新条目只能有 record_id 和 fields。"
            )
        record_id = _record_id(record["record_id"], index)
        if record_id in seen:
            raise FieldError(f"第 {index + 1} 个操作重复更新 {record_id}。")
        seen.add(record_id)
        parsed.append({"record_id": record_id, "fields": _fields_object(record["fields"], index)})
    return {"op": "update", "records": parsed}


def _parse_delete(operation: dict, index: int) -> dict:
    _require_keys(operation, _DELETE_FORMS, index)
    if "record_id" in operation:
        return {"op": "delete", "record_ids": [_record_id(operation["record_id"], index)]}
    record_ids = operation["record_ids"]
    if not isinstance(record_ids, list) or not record_ids:
        raise FieldError(f"第 {index + 1} 个操作的 record_ids 必须是非空数组。")
    parsed = [_record_id(record_id, index) for record_id in record_ids]
    if len(parsed) != len(set(parsed)):
        raise FieldError(f"第 {index + 1} 个操作的 record_ids 有重复。")
    return {"op": "delete", "record_ids": parsed}


def _parse_upsert(operation: dict, index: int) -> dict:
    _require_keys(operation, _UPSERT_FORMS, index)
    key_field = operation["key_field"]
    if not isinstance(key_field, str) or not key_field:
        raise FieldError(f"第 {index + 1} 个操作的 key_field 必须是非空字符串。")
    if "fields" in operation:
        records = [{"fields": _fields_object(operation["fields"], index)}]
    else:
        records = _field_records(operation["records"], index)
    return {"op": "upsert", "key_field": key_field, "records": records}


def _require_keys(operation: dict, allowed: set, index: int) -> None:
    if set(operation) not in allowed:
        raise FieldError(f"第 {index + 1} 个操作的字段组合不符合规格。")


def _fields_object(value: object, index: int) -> dict:
    if not isinstance(value, dict) or not value:
        raise FieldError(f"第 {index + 1} 个操作的 fields 必须是非空对象。")
    return value


def _field_records(value: object, index: int) -> list[dict]:
    if not isinstance(value, list) or not value:
        raise FieldError(f"第 {index + 1} 个操作的 records 必须是非空数组。")
    records = []
    for record in value:
        if not isinstance(record, dict) or set(record) != {"fields"}:
            raise FieldError(f"第 {index + 1} 个操作的记录只能有 fields。")
        records.append({"fields": _fields_object(record["fields"], index)})
    return records


def _record_id(value: object, index: int) -> str:
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise FieldError(f"第 {index + 1} 个操作的 record_id 必须是不含首尾空白的非空字符串。")
    return value


def _record_fields(operation: dict) -> list[dict]:
    return [record["fields"] for record in operation["records"]]


def _update_records(operation: dict) -> list[dict]:
    return operation["records"]


def _delete_ids(operation: dict) -> list[str]:
    return operation["record_ids"]
