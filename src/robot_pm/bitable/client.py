"""Feishu Bitable Open API adapter.

A later agent can call this module without going through the CLI. Table
identity comes from ``BitableConfig``. Field names come from the caller.
"""

from __future__ import annotations

import json
import time
from collections.abc import Iterator
from urllib.parse import quote

import httpx

from robot_pm.bitable.config import BitableConfig
from robot_pm.bitable.errors import (
    AuthError,
    BitableAPIError,
    PartialBatchError,
    RateLimitError,
    scrub,
)
from robot_pm.bitable.fields import FieldCatalog

BATCH_CREATE_LIMIT = 1000
BATCH_UPDATE_LIMIT = 1000
BATCH_DELETE_LIMIT = 500
_RATE_LIMIT_CODES = {1254290, 1254291, 1254112}


def build_http_client(base_url: str) -> httpx.Client:
    return httpx.Client(
        base_url=base_url,
        timeout=httpx.Timeout(30.0),
        trust_env=False,
    )


class BitableClient:
    """Calls bitable v1 with a tenant_access_token fetched at runtime."""

    def __init__(
        self,
        config: BitableConfig,
        http: httpx.Client | None = None,
    ) -> None:
        self._config = config
        self._owns_http = http is None
        self._http = http if http is not None else build_http_client(config.base_url)
        self._token = ""
        self._token_deadline = 0.0
        self._catalog: FieldCatalog | None = None

    def close(self) -> None:
        if self._owns_http:
            self._http.close()

    def __enter__(self) -> BitableClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def field_catalog(self) -> FieldCatalog:
        if self._catalog is None:
            self._catalog = FieldCatalog.from_items(self.list_fields())
        return self._catalog

    def list_fields(self) -> list[dict]:
        items: list[dict] = []
        page_token = ""
        while True:
            params: dict[str, str | int] = {"page_size": 100}
            if page_token:
                params["page_token"] = page_token
            data = self._bitable(
                "GET",
                f"{self._table_path}/fields",
                params=params,
                error_label="列出字段",
            )
            page_items = data.get("items") or []
            if not isinstance(page_items, list):
                raise self._api("列出字段的 items 不是数组")
            items.extend(page_items)
            if not data.get("has_more"):
                return items
            page_token = data.get("page_token") or ""
            if not isinstance(page_token, str) or not page_token:
                raise self._api("列出字段声明还有下一页但没有 page_token")

    def search_record_ids(self, field_name: str, literal: str) -> list[str]:
        """Return record ids whose field equals ``literal``.

        Uses one search page of size 2. More than one hit is returned as-is
        so the caller can refuse an ambiguous upsert. This does not write.
        """
        data = self._bitable(
            "POST",
            f"{self._records_path}/search",
            params={"page_size": 2},
            json_body={
                "field_names": [field_name],
                "filter": {
                    "conjunction": "and",
                    "conditions": [
                        {
                            "field_name": field_name,
                            "operator": "is",
                            "value": [literal],
                        }
                    ],
                },
            },
            error_label="搜索记录",
        )
        raw_items = data.get("items") or []
        if not isinstance(raw_items, list):
            raise self._api("搜索记录的 items 不是数组")
        if len(raw_items) == 0 and data.get("has_more"):
            raise self._api("搜索结果不一致：还有更多记录但本页是空的")
        if len(raw_items) == 1 and data.get("has_more"):
            raise self._api("搜索结果不一致：还有更多记录但本页只有 1 条")
        ids: list[str] = []
        for item in raw_items:
            if not isinstance(item, dict):
                raise self._api("搜索记录的条目不是对象")
            record_id = item.get("record_id") or item.get("id")
            if not isinstance(record_id, str) or not record_id:
                raise self._api("搜索记录的返回缺少 record_id")
            ids.append(record_id)
        return ids

    def create_record(self, fields: dict) -> str:
        self.field_catalog().check_record(fields)
        data = self._bitable(
            "POST",
            self._records_path,
            json_body={"fields": fields},
            error_label="新增记录",
        )
        return _one_record_id(data, self._api)

    def batch_create_records(self, records: list[dict]) -> list[str]:
        for fields in records:
            self.field_catalog().check_record(fields)
        created: list[str] = []
        for chunk in _chunks(records, BATCH_CREATE_LIMIT):
            try:
                data = self._bitable(
                    "POST",
                    f"{self._records_path}/batch_create",
                    json_body={"records": [{"fields": fields} for fields in chunk]},
                    error_label="新增多条记录",
                )
            except BitableAPIError as exc:
                if created:
                    raise self._partial(
                        "批量新增在后续分批失败。失败的那一批只有整批 code，"
                        "没有逐条回执，不能判断该批内部哪些行已落库。"
                        f"此前分批已返回 {len(created)} 条 record_id。",
                        applied=created,
                        rejected=[],
                    ) from exc
                raise
            created.extend(_many_record_ids(data, self._api, expected=len(chunk)))
        return created

    def update_record(self, record_id: str, fields: dict) -> str:
        self.field_catalog().check_record(fields)
        data = self._bitable(
            "PUT",
            f"{self._records_path}/{quote(record_id, safe='')}",
            json_body={"fields": fields},
            error_label="更新记录",
        )
        return _one_record_id(data, self._api)

    def batch_update_records(self, records: list[dict]) -> list[str]:
        for record in records:
            self.field_catalog().check_record(record["fields"])
        updated: list[str] = []
        for chunk in _chunks(records, BATCH_UPDATE_LIMIT):
            try:
                data = self._bitable(
                    "POST",
                    f"{self._records_path}/batch_update",
                    json_body={"records": chunk},
                    error_label="更新多条记录",
                )
            except BitableAPIError as exc:
                if updated:
                    raise self._partial(
                        "批量更新在后续分批失败。失败的那一批只有整批 code，"
                        "没有逐条回执。"
                        f"此前分批已返回 {len(updated)} 条 record_id。",
                        applied=updated,
                        rejected=[record["record_id"] for record in chunk],
                    ) from exc
                raise
            updated.extend(_many_record_ids(data, self._api, expected=len(chunk)))
        return updated

    def delete_record(self, record_id: str) -> str:
        data = self._bitable(
            "DELETE",
            f"{self._records_path}/{quote(record_id, safe='')}",
            error_label="删除记录",
        )
        deleted = data.get("deleted")
        returned = data.get("record_id") or record_id
        if deleted is not True or not isinstance(returned, str):
            raise self._api(f"删除记录未成功：{record_id}")
        return returned

    def batch_delete_records(self, record_ids: list[str]) -> list[str]:
        deleted: list[str] = []
        for chunk in _chunks(record_ids, BATCH_DELETE_LIMIT):
            try:
                data = self._bitable(
                    "POST",
                    f"{self._records_path}/batch_delete",
                    json_body={"records": chunk},
                    error_label="删除多条记录",
                )
            except BitableAPIError as exc:
                if deleted:
                    raise self._partial(
                        "批量删除在后续分批失败。失败的那一批没有成功回执。"
                        f"此前分批已删除 {len(deleted)} 条。",
                        applied=deleted,
                        rejected=list(chunk),
                    ) from exc
                raise
            ok, bad = _deleted_flags(data, chunk, self._api)
            deleted.extend(ok)
            if bad:
                shown = "、".join(bad[:20])
                if len(bad) > 20:
                    shown += " 等"
                raise self._partial(
                    "批量删除只有部分记录成功。"
                    f"已删除 {len(deleted)} 条，未删除 {len(bad)} 条。"
                    f"未删除的 record_id：{shown}。",
                    applied=deleted,
                    rejected=bad,
                )
        return deleted

    @property
    def _table_path(self) -> str:
        app = quote(self._config.app_token, safe="")
        table = quote(self._config.table_id, safe="")
        return f"/open-apis/bitable/v1/apps/{app}/tables/{table}"

    @property
    def _records_path(self) -> str:
        return f"{self._table_path}/records"

    def _bitable(self, method: str, path: str, **kwargs: object) -> dict:
        body = self._request(method, path, auth=True, **kwargs)
        data = body.get("data")
        if not isinstance(data, dict):
            raise self._api(f"{kwargs.get('error_label', '接口')} 的响应缺少 data")
        return data

    def _request(
        self,
        method: str,
        path: str,
        *,
        auth: bool,
        params: dict | None = None,
        json_body: dict | None = None,
        error_label: str,
        auth_call: bool = False,
    ) -> dict:
        headers: dict[str, str] = {}
        content = None
        if auth:
            headers["Authorization"] = f"Bearer {self._access_token()}"
        if json_body is not None:
            headers["Content-Type"] = "application/json; charset=utf-8"
            content = json.dumps(json_body, ensure_ascii=False, separators=(",", ":")).encode(
                "utf-8"
            )
        try:
            response = self._http.request(
                method,
                path,
                params=params,
                content=content,
                headers=headers,
            )
        except httpx.HTTPError as exc:
            message = self._scrub(f"{error_label} 请求失败：{exc.__class__.__name__}")
            if auth_call:
                raise AuthError(message, secrets=self._secrets()) from None
            raise self._api(message) from None
        return self._parse_response(response, error_label=error_label, auth_call=auth_call)

    def _access_token(self) -> str:
        now = time.monotonic()
        if self._token and now < self._token_deadline:
            return self._token
        body = self._request(
            "POST",
            "/open-apis/auth/v3/tenant_access_token/internal",
            auth=False,
            json_body={
                "app_id": self._config.app_id,
                "app_secret": self._config.app_secret,
            },
            error_label="获取 tenant_access_token",
            auth_call=True,
        )
        token = body.get("tenant_access_token")
        expire = body.get("expire")
        if not isinstance(token, str) or not token:
            raise AuthError(
                "获取 tenant_access_token 的响应里没有凭证",
                secrets=self._secrets(),
            )
        if isinstance(expire, bool) or not isinstance(expire, int):
            raise AuthError(
                "获取 tenant_access_token 的响应缺少 expire",
                secrets=self._secrets(),
            )
        skew = 60 if expire > 120 else 0
        self._token = token
        self._token_deadline = now + max(expire - skew, 1)
        return token

    def _parse_response(
        self,
        response: httpx.Response,
        *,
        error_label: str,
        auth_call: bool,
    ) -> dict:
        try:
            body = response.json()
        except json.JSONDecodeError:
            body = None
        if not isinstance(body, dict):
            message = f"{error_label} 的响应不是 JSON 对象（HTTP {response.status_code}）"
            if auth_call or response.status_code == 401:
                raise AuthError(self._scrub(message), secrets=self._secrets())
            raise self._api(message, http_status=response.status_code)
        code = body.get("code")
        msg = self._scrub(str(body.get("msg") or ""))
        if response.status_code == 401 or (auth_call and code != 0):
            detail = f"{error_label} 失败"
            if isinstance(code, int) and not isinstance(code, bool):
                detail += f"（code={code}）"
            if msg:
                detail += f"：{msg}"
            raise AuthError(detail, secrets=self._secrets())
        if response.status_code == 429 or code in _RATE_LIMIT_CODES:
            raise RateLimitError(
                f"{error_label} 被限流（HTTP {response.status_code}, code={code}）。"
                "本程序不会自动重试。请稍后单独重试；已经成功的操作不会回滚。",
                code=code if isinstance(code, int) else None,
                http_status=response.status_code,
                secrets=self._secrets(),
            )
        if code != 0:
            detail = f"{error_label} 失败（HTTP {response.status_code}, code={code}）"
            if msg:
                detail += f"：{msg}"
            raise self._api(detail, code=code if isinstance(code, int) else None, http_status=response.status_code)
        if response.status_code >= 400:
            raise self._api(
                f"{error_label} 失败（HTTP {response.status_code}）",
                http_status=response.status_code,
            )
        return body

    def _secrets(self) -> tuple[str, ...]:
        values = list(self._config.secret_values())
        if self._token:
            values.append(self._token)
        return tuple(values)

    def _scrub(self, text: str) -> str:
        return scrub(text, self._secrets())

    def _api(
        self,
        message: str,
        *,
        code: int | None = None,
        http_status: int | None = None,
    ) -> BitableAPIError:
        return BitableAPIError(
            self._scrub(message),
            code=code,
            http_status=http_status,
            secrets=self._secrets(),
        )

    def _partial(self, message: str, *, applied: list[str], rejected: list[str]) -> PartialBatchError:
        return PartialBatchError(
            self._scrub(message),
            applied=applied,
            rejected=rejected,
            secrets=self._secrets(),
        )


def _chunks(items: list, size: int) -> Iterator[list]:
    for start in range(0, len(items), size):
        yield items[start : start + size]


def _one_record_id(data: dict, api_error) -> str:
    record = data.get("record")
    if not isinstance(record, dict):
        raise api_error("响应缺少 record")
    record_id = record.get("record_id") or record.get("id")
    if not isinstance(record_id, str) or not record_id:
        raise api_error("响应缺少 record_id")
    return record_id


def _many_record_ids(data: dict, api_error, *, expected: int) -> list[str]:
    records = data.get("records")
    if not isinstance(records, list):
        raise api_error("响应缺少 records")
    ids: list[str] = []
    for record in records:
        if not isinstance(record, dict):
            raise api_error("响应中的记录不是对象")
        record_id = record.get("record_id") or record.get("id")
        if not isinstance(record_id, str) or not record_id:
            raise api_error("响应中的记录缺少 record_id")
        ids.append(record_id)
    if len(ids) != expected:
        raise api_error(f"响应记录数为 {len(ids)}，请求为 {expected}")
    return ids


def _deleted_flags(data: dict, requested: list[str], api_error) -> tuple[list[str], list[str]]:
    records = data.get("records")
    if not isinstance(records, list):
        raise api_error("批量删除的响应缺少 records")
    ok: list[str] = []
    bad: list[str] = []
    seen: set[str] = set()
    for record in records:
        if not isinstance(record, dict):
            raise api_error("批量删除的记录回执不是对象")
        record_id = record.get("record_id")
        if not isinstance(record_id, str) or not record_id:
            raise api_error("批量删除的回执缺少 record_id")
        seen.add(record_id)
        if record.get("deleted") is True:
            ok.append(record_id)
        else:
            bad.append(record_id)
    for record_id in requested:
        if record_id not in seen:
            bad.append(record_id)
    return ok, bad
