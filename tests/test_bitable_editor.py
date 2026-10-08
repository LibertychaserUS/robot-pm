"""HTTP is mocked. These tests do not call Feishu."""

from __future__ import annotations

import json
from pathlib import Path

import httpx
import pytest

from robot_pm.bitable.client import BitableClient
from robot_pm.bitable.config import BitableConfig
from robot_pm.bitable.editor import apply_edit
from robot_pm.bitable.errors import AuthError, BitableAPIError, EditFailed, FieldError
from robot_pm.bitable.__main__ import main

APP_SECRET = "app-secret-SHOULD-NOT-LEAK-xyz"
TOKEN = "t-token-SHOULD-NOT-LEAK-xyz"
APP_TOKEN = "app_token_example"
TABLE_ID = "tbl_example"

FIELDS = [
    {"field_id": "fld_code", "field_name": "fake_item_code", "type": 1, "ui_type": "Text"},
    {"field_id": "fld_title", "field_name": "fake_title", "type": 1, "ui_type": "Text"},
    {"field_id": "fld_count", "field_name": "fake_count", "type": 2, "ui_type": "Number"},
    {"field_id": "fld_done", "field_name": "fake_done", "type": 7, "ui_type": "Checkbox"},
    {"field_id": "fld_formula", "field_name": "fake_formula", "type": 20, "ui_type": "Formula"},
]


def _config() -> BitableConfig:
    return BitableConfig(
        app_id="cli_example",
        app_secret=APP_SECRET,
        app_token=APP_TOKEN,
        table_id=TABLE_ID,
        base_url="https://open.feishu.cn",
    )


def _is_write(request: httpx.Request) -> bool:
    path = request.url.path
    if path.endswith("/records/search") or path.endswith("/fields"):
        return False
    if path.endswith("/tenant_access_token/internal"):
        return False
    if request.method in {"PUT", "DELETE"}:
        return True
    return request.method == "POST" and "/records" in path


def _client(handler, calls: list[httpx.Request]) -> tuple[BitableClient, httpx.Client]:
    def wrapped(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        return handler(request)

    http = httpx.Client(
        transport=httpx.MockTransport(wrapped),
        base_url="https://open.feishu.cn",
        trust_env=False,
    )
    return BitableClient(_config(), http=http), http


def _ok(data: dict) -> httpx.Response:
    return httpx.Response(200, json={"code": 0, "msg": "success", "data": data})


def _route(request: httpx.Request, *, search_items: list[dict] | None = None) -> httpx.Response:
    path = request.url.path
    if path.endswith("/tenant_access_token/internal"):
        return httpx.Response(
            200,
            json={
                "code": 0,
                "msg": "ok",
                "tenant_access_token": TOKEN,
                "expire": 7200,
            },
        )
    if path.endswith("/fields"):
        return _ok({"has_more": False, "items": FIELDS})
    if path.endswith("/records/search"):
        return _ok({"has_more": False, "items": search_items or []})
    if request.method == "POST" and path.endswith("/records"):
        return _ok({"record": {"record_id": "recNEW", "id": "recNEW"}})
    if path.endswith("/records/batch_create"):
        body = json.loads(request.content)
        records = [
            {"record_id": f"recNEW{index}", "id": f"recNEW{index}"}
            for index in range(len(body["records"]))
        ]
        return _ok({"records": records})
    if request.method == "PUT":
        return _ok({"record": {"record_id": path.rsplit("/", 1)[-1], "id": path.rsplit("/", 1)[-1]}})
    if path.endswith("/records/batch_delete"):
        body = json.loads(request.content)
        return _ok(
            {
                "records": [
                    {"deleted": True, "record_id": record_id} for record_id in body["records"]
                ]
            }
        )
    if request.method == "DELETE":
        return _ok({"deleted": True, "record_id": path.rsplit("/", 1)[-1]})
    raise AssertionError(f"unexpected request {request.method} {path}")


def test_dry_run_performs_no_write() -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        return _route(
            request,
            search_items=[{"record_id": "recMATCHED", "id": "recMATCHED"}],
        )

    client, http = _client(handler, calls)
    try:
        result = apply_edit(
            client,
            {
                "operations": [
                    {
                        "op": "upsert",
                        "key_field": "fake_item_code",
                        "fields": {
                            "fake_item_code": "DEMO-001",
                            "fake_title": "planned",
                        },
                    },
                    {"op": "create", "fields": {"fake_title": "also planned"}},
                ]
            },
            dry_run=True,
        )
    finally:
        http.close()

    assert result["dry_run"] is True
    assert result["operations"][0]["rows"][0]["action"] == "update"
    assert result["operations"][0]["rows"][0]["record_id"] == "recMATCHED"
    assert calls, "dry-run still reads field metadata"
    assert not any(_is_write(request) for request in calls)


def test_upsert_matches_key_field() -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        return _route(
            request,
            search_items=[{"record_id": "recMATCHED", "id": "recMATCHED"}],
        )

    client, http = _client(handler, calls)
    try:
        result = apply_edit(
            client,
            {
                "operations": [
                    {
                        "op": "upsert",
                        "key_field": "fake_item_code",
                        "fields": {
                            "fake_item_code": "DEMO-001",
                            "fake_title": "updated example",
                        },
                    }
                ]
            },
            dry_run=False,
        )
    finally:
        http.close()

    searches = [request for request in calls if request.url.path.endswith("/records/search")]
    assert len(searches) == 1
    search_body = json.loads(searches[0].content)
    condition = search_body["filter"]["conditions"][0]
    assert condition["field_name"] == "fake_item_code"
    assert condition["operator"] == "is"
    assert condition["value"] == ["DEMO-001"]

    writes = [request for request in calls if _is_write(request)]
    assert len(writes) == 1
    assert writes[0].method == "PUT"
    assert writes[0].url.path.endswith("/records/recMATCHED")
    assert json.loads(writes[0].content)["fields"]["fake_item_code"] == "DEMO-001"
    assert result["operations"][0]["rows"][0]["action"] == "update"
    assert result["operations"][0]["rows"][0]["record_id"] == "recMATCHED"


def test_upsert_creates_when_key_is_absent() -> None:
    calls: list[httpx.Request] = []
    client, http = _client(_route, calls)
    try:
        apply_edit(
            client,
            {
                "operations": [
                    {
                        "op": "upsert",
                        "key_field": "fake_item_code",
                        "fields": {"fake_item_code": "DEMO-404", "fake_title": "new"},
                    }
                ]
            },
            dry_run=False,
        )
    finally:
        http.close()

    writes = [request for request in calls if _is_write(request)]
    assert len(writes) == 1
    assert writes[0].method == "POST"
    assert writes[0].url.path.endswith("/records")
    assert not writes[0].url.path.endswith("/records/search")


def test_unsupported_type_is_rejected_before_write() -> None:
    calls: list[httpx.Request] = []
    client, http = _client(_route, calls)
    try:
        with pytest.raises(FieldError, match="不支持的字段类型"):
            apply_edit(
                client,
                {
                    "operations": [
                        {"op": "create", "fields": {"fake_title": "would have been written"}},
                        {"op": "update", "record_id": "recEXAMPLE0001", "fields": {"fake_formula": "nope"}},
                    ]
                },
                dry_run=False,
            )
    finally:
        http.close()

    assert not any(_is_write(request) for request in calls)


def test_unknown_field_is_rejected_before_write() -> None:
    calls: list[httpx.Request] = []
    client, http = _client(_route, calls)
    try:
        with pytest.raises(FieldError, match="未知字段"):
            apply_edit(
                client,
                {"operations": [{"op": "create", "fields": {"not_a_real_column": "x"}}]},
                dry_run=False,
            )
    finally:
        http.close()
    assert not any(_is_write(request) for request in calls)


def test_wrong_value_type_is_rejected_before_write() -> None:
    calls: list[httpx.Request] = []
    client, http = _client(_route, calls)
    try:
        with pytest.raises(FieldError, match="需要数字"):
            apply_edit(
                client,
                {"operations": [{"op": "create", "fields": {"fake_count": "twelve"}}]},
                dry_run=False,
            )
    finally:
        http.close()
    assert not any(_is_write(request) for request in calls)


def test_auth_header_is_set() -> None:
    calls: list[httpx.Request] = []
    client, http = _client(_route, calls)
    try:
        apply_edit(
            client,
            {"operations": [{"op": "create", "fields": {"fake_title": "header check"}}]},
            dry_run=False,
        )
    finally:
        http.close()

    token_calls = [
        request for request in calls if request.url.path.endswith("/tenant_access_token/internal")
    ]
    bitable_calls = [
        request for request in calls if not request.url.path.endswith("/tenant_access_token/internal")
    ]
    assert token_calls
    assert token_calls[0].headers.get("Authorization") is None
    assert bitable_calls
    assert all(request.headers.get("Authorization") == f"Bearer {TOKEN}" for request in bitable_calls)


def test_secrets_do_not_appear_in_auth_error() -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200,
            json={"code": 10014, "msg": f"rejected {APP_SECRET}"},
        )

    client, http = _client(handler, calls)
    try:
        with pytest.raises(AuthError) as caught:
            client.list_fields()
    finally:
        http.close()

    text = str(caught.value)
    assert APP_SECRET not in text
    assert TOKEN not in text
    assert "10014" in text


def test_secrets_do_not_appear_in_api_error() -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path.endswith("/tenant_access_token/internal"):
            return httpx.Response(
                200,
                json={
                    "code": 0,
                    "msg": "ok",
                    "tenant_access_token": TOKEN,
                    "expire": 7200,
                },
            )
        return httpx.Response(
            200,
            json={"code": 1254002, "msg": f"echo {TOKEN} and {APP_SECRET} and {APP_TOKEN}"},
        )

    client, http = _client(handler, calls)
    try:
        with pytest.raises(BitableAPIError) as caught:
            client.list_fields()
    finally:
        http.close()

    text = str(caught.value)
    assert TOKEN not in text
    assert APP_SECRET not in text
    assert APP_TOKEN not in text


def test_partial_batch_delete_stops_later_operations() -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path.endswith("/records/batch_delete"):
            return _ok(
                {
                    "records": [
                        {"deleted": True, "record_id": "recKEEP"},
                        {"deleted": False, "record_id": "recMISS"},
                    ]
                }
            )
        return _route(request)

    client, http = _client(handler, calls)
    try:
        with pytest.raises(EditFailed, match="部分失败") as caught:
            apply_edit(
                client,
                {
                    "operations": [
                        {"op": "delete", "record_ids": ["recKEEP", "recMISS"]},
                        {"op": "create", "fields": {"fake_title": "must not be created"}},
                    ]
                },
                dry_run=False,
            )
    finally:
        http.close()

    assert caught.value.completed[0]["status"] == "partial"
    assert "recMISS" in caught.value.completed[0]["not_applied"]
    assert not any(request.method == "POST" and request.url.path.endswith("/records") for request in calls)


def test_cli_dry_run(monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        return _route(request, search_items=[])

    monkeypatch.setenv("FEISHU_APP_ID", "cli_example")
    monkeypatch.setenv("FEISHU_APP_SECRET", APP_SECRET)
    monkeypatch.setenv("FEISHU_BITABLE_APP_TOKEN", APP_TOKEN)
    monkeypatch.setenv("FEISHU_BITABLE_TABLE_ID", TABLE_ID)
    monkeypatch.setattr(
        "robot_pm.bitable.client.build_http_client",
        lambda base_url: httpx.Client(
            transport=httpx.MockTransport(handler),
            base_url=base_url,
            trust_env=False,
        ),
    )
    edit = tmp_path / "edit.json"
    edit.write_text(
        json.dumps(
            {
                "operations": [
                    {
                        "op": "upsert",
                        "key_field": "fake_item_code",
                        "fields": {"fake_item_code": "DEMO-001", "fake_title": "dry"},
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    assert main(["写入", str(edit), "--只检查"]) == 0
    captured = capsys.readouterr()
    assert '"dry_run": true' in captured.out
    assert APP_SECRET not in captured.out
    assert TOKEN not in captured.out
    assert not any(_is_write(request) for request in calls)


def test_example_edit_file_uses_fake_names() -> None:
    example = Path(__file__).resolve().parents[1] / "examples" / "edit.example.json"
    document = json.loads(example.read_text(encoding="utf-8"))
    kinds = [operation["op"] for operation in document["operations"]]
    assert kinds == ["upsert", "create", "update", "delete"]
    assert document["operations"][0]["key_field"] == "fake_item_code"
    assert document["operations"][2]["record_id"].startswith("recEXAMPLE")
