# 多维表格编辑规格

本规格描述 robot PM 当前这一刀：一个与具体业务表无关的飞书多维表格编辑程序。评审和实现都以本文为准。接口形状来自飞书开放平台文档，不另造端点。

## 非目标

本程序不做这些事：

- 不盯人、不拉会、不发进度报告，也不实现飞书机器人或群聊命令。
- 不规定需求表、缺陷表或任何业务字段。字段名只来自编辑文件，表身份只来自环境变量。
- 不写死租户、群 chat id、app id、app secret、app token、table id。
- 不创建或修改字段、数据表、视图、仪表盘，不上传附件。附件字段只接受已经存在的 `file_token`。
- 不自动重试。限流、冲突、超时都返回错误，由调用方决定是否重试。
- 不发送飞书的 `client_token` 幂等键。幂等只来自本文的 upsert 键字段。
- 不读取仓库里的 `.env`。进程环境里没有变量就失败。

## 配置、编辑文件、代码各管什么

| 内容 | 放在哪里 | 例子 |
| --- | --- | --- |
| 应用凭证、多维表格 app token、数据表 table id、开放平台域名 | 环境变量 | `FEISHU_APP_ID` |
| 要执行的操作和字段值 | 编辑 JSON | `examples/edit.example.json` |
| 如何取 token、如何映射字段、如何调用接口 | 代码 | `robot_pm.bitable` |

环境变量：

| 变量 | 必填 | 含义 |
| --- | --- | --- |
| `FEISHU_APP_ID` | 是 | 自建应用 app id |
| `FEISHU_APP_SECRET` | 是 | 自建应用 app secret |
| `FEISHU_BITABLE_APP_TOKEN` | 是 | 多维表格 app token |
| `FEISHU_BITABLE_TABLE_ID` | 是 | 数据表 table id |
| `FEISHU_BASE_URL` | 否 | 默认 `https://open.feishu.cn`，去掉末尾斜杠 |

空字符串视为缺失。错误信息只列出缺失的变量名，不回显变量值。

适配器 `BitableClient` 只依赖上述配置。编辑程序把编辑文件交给适配器。以后的 agent 可以直接调用 `BitableClient`，不必走命令行。

## 鉴权

使用自建应用获取 `tenant_access_token`：

- `POST {FEISHU_BASE_URL}/open-apis/auth/v3/tenant_access_token/internal`
- 请求体：`{"app_id","app_secret"}`
- 成功时响应顶层有 `tenant_access_token` 和 `expire`（秒）

文档：[自建应用获取 tenant_access_token](https://open.feishu.cn/document/server-docs/authentication-management/access-token/tenant_access_token_internal)。

之后每个多维表格请求带请求头 `Authorization: Bearer <tenant_access_token>`，以及在有请求体时带 `Content-Type: application/json; charset=utf-8`。取 token 的请求本身不带 Authorization。

凭证在内存中缓存到过期前。剩余有效期按响应里的 `expire` 计算，大于 120 秒时提前 60 秒刷新。不把完整 token 写进日志、异常文本或仓库。异常文本里如果出现 app secret、app id、app token 或已经拿到的 tenant_access_token，一律替换成 `[redacted]`。

取 token 失败（响应不是成功 JSON，或 `code != 0`，或 HTTP 401）是鉴权错误，程序停止，不写表。

## 编辑 JSON

文件编码 UTF-8。顶层只能有 `operations`，值为非空数组。操作按数组顺序执行。

每个操作的 `op` 只能是 `create`、`update`、`delete` 或 `upsert`。除下面列出的键以外，多一个或少一个都拒绝。

### create

单条：

```json
{"op": "create", "fields": {"fake_title": "example"}}
```

多条：

```json
{
  "op": "create",
  "records": [
    {"fields": {"fake_title": "batch example A"}},
    {"fields": {"fake_title": "batch example B"}}
  ]
}
```

`fields` 与 `records` 只能有一个。`fields` 必须是非空对象。`records` 的每一项只能有 `fields`。

一条走[新增记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/create)：`POST /open-apis/bitable/v1/apps/:app_token/tables/:table_id/records`，请求体 `{"fields": {...}}`。

多条走[新增多条记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_create)：`POST .../records/batch_create`，请求体 `{"records":[{"fields":{...}}]}`。单次最多 1000 条，超出则按 1000 条分批，按原顺序。

### update

单条：

```json
{"op": "update", "record_id": "recEXAMPLE0001", "fields": {"fake_title": "renamed example"}}
```

多条：

```json
{
  "op": "update",
  "records": [
    {"record_id": "recEXAMPLE0001", "fields": {"fake_title": "renamed example"}}
  ]
}
```

更新是增量的：只提交给出的字段。把某个已支持字段设为 JSON `null` 表示清空。文档：[更新记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/update) 中的置空示例。

一条走 `PUT .../records/:record_id`。多条走[更新多条记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_update)：`POST .../records/batch_update`，请求体 `{"records":[{"record_id","fields"}]}`。单次最多 1000 条，超出按 1000 条分批。同一操作里 `record_id` 不能重复。

### delete

单条：`{"op": "delete", "record_id": "recEXAMPLE0002"}`，走 `DELETE .../records/:record_id`。文档：[删除记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/delete)。

多条：`{"op": "delete", "record_ids": ["recEXAMPLE0002", "recEXAMPLE0003"]}`，走[删除多条记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_delete)：`POST .../records/batch_delete`，请求体 `{"records":["rec..."]}`。单次最多 500 条，超出按 500 条分批。`record_id` 不能重复，不能是空白。

### upsert

```json
{
  "op": "upsert",
  "key_field": "fake_item_code",
  "fields": {"fake_item_code": "DEMO-001", "fake_title": "example row"}
}
```

也可以用 `records` 代替 `fields`，形状与 create 的 `records` 相同。每条记录的 `fields` 都必须包含 `key_field`。

`key_field` 是字段名，不是 field id。它必须是当前表里的字段，而且类型必须是下面「可做键」的类型。键值不能是 `null` 或空字符串。

查找使用[查询记录](https://open.feishu.cn/document/docs/bitable-v1/app-table-record/search)：

`POST .../records/search?page_size=2`

```json
{
  "field_names": ["fake_item_code"],
  "filter": {
    "conjunction": "and",
    "conditions": [
      {"field_name": "fake_item_code", "operator": "is", "value": ["DEMO-001"]}
    ]
  }
}
```

筛选值是字符串数组，见[记录筛选参数填写说明](https://open.feishu.cn/document/docs/bitable-v1/app-table-record/record-filter-guide)。数字键会先变成十进制字符串再放进 `value`：整数 `100` 变成 `"100"`；非整数用普通十进制并去掉末尾 0，例如 `0.25` 变成 `"0.25"`，不用科学计数法。

判定：

| 命中条数 | 行为 |
| --- | --- |
| 0 | create |
| 1 | 用返回的 `record_id` 做 update，不新增 |
| 大于 1 | 报错。不更新其中任何一条，本条 upsert 不写入 |

`page_size` 为 2。本页超过 1 条就视为不唯一，不再翻页。

同一条 upsert 里，同一个键出现多次：只创建或匹配一次，后面的字段值再更新那一条，后出现的字段值为准。同一编辑文件里，前面 upsert 已经决定的键记在本次进程的内存里，后面的 upsert 直接用那个 `record_id`，避免刚写完的行还搜不到。普通 create 不进入这张键表。不要紧挨着一条普通 create，再用 upsert 去匹配刚写进去的同一个键。

本程序不保证两个进程同时 upsert 同一个键时只有一条记录。飞书对同一数据表的写调用本身也不支持并发（错误码 1254291）。

## 写入前的字段检查

写之前先列出字段，并翻页直到 `has_more` 为假：

`GET /open-apis/bitable/v1/apps/:app_token/tables/:table_id/fields?page_size=100`

文档：[列出字段](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-field/list)。

整个编辑文件先对照字段元数据检查，再执行任何写操作。因此后面一个操作的未知字段或错误类型，会使前面的 create 也不执行。

- 字段名必须与 `field_name` 完全一致。不在表里的名字报「未知字段」，不写。
- `type` + `ui_type` 不在下表中的，报「不支持的字段类型」，不写。列出字段没给 `ui_type` 时，才按 `type` 填默认 ui：1 文本、2 数字、3 单选、4 多选、5 日期、7 复选框、11 人员、13 电话、15 超链接、17 附件、18 单向关联、21 双向关联、22 地理位置、23 群组。
- 值的 JSON 类型不符合该字段的写入格式时，报该字段需要的形状，不写。

`null` 对已支持字段表示清空，不再检查具体形状。键字段不允许 `null`。

## 支持的字段写入格式

写入格式以新增/更新记录的请求体为准，不以查询结果的读取格式为准。读取格式见[多维表格记录数据结构](https://open.feishu.cn/document/docs/bitable-v1/app-table-record/bitable-record-data-structure-overview)。两边不一致的地方在表中写明。

| ui_type | type | 可做键 | 编辑文件里的值 |
| --- | --- | --- | --- |
| Text | 1 | 是 | 字符串。读取时是 `[{text,type}]`，写入不要用那个数组 |
| Barcode | 1 | 是 | 字符串。数据结构说明 type 1 写入时为 string |
| Email | 1 | 是 | 字符串。同上 |
| Number | 2 | 是 | 数字。不要用 `true`/`false` |
| Progress | 2 | 是 | 数字，例如 `0.25` |
| Currency | 2 | 是 | 数字 |
| Rating | 2 | 是 | 数字 |
| SingleSelect | 3 | 是 | 选项名字符串 |
| MultiSelect | 4 | 否 | 非空字符串数组 |
| DateTime | 5 | 否 | 毫秒级 Unix 时间戳，JSON 整数 |
| Checkbox | 7 | 否 | `true` 或 `false` |
| User | 11 | 否 | `[{"id":"ou_..."}]`，对象里只能有 `id`。默认 `user_id_type` 是 `open_id`，本程序不另传该查询参数 |
| Phone | 13 | 是 | 字符串，匹配 `(\+)?\d*`，最长 64。数据结构文档给出该正则 |
| Url | 15 | 否 | `{"text":"...","link":"..."}`，两个键都必须在，且都是非空字符串 |
| Attachment | 17 | 否 | `[{"file_token":"..."}]`，对象里只能有 `file_token` |
| SingleLink | 18 | 否 | 非空 record_id 字符串数组。读取时是 `{link_record_ids:[...]}`，写入不要用那个对象 |
| DuplexLink | 21 | 否 | 非空 record_id 字符串数组。错误码 1254074 要求字符串数组 |
| Location | 22 | 否 | 字符串 `"经度,纬度"`，例如 `"116.397755,39.903179"`。读取时是地址对象，写入不要用那个对象 |
| GroupChat | 23 | 否 | `[{"id":"oc_..."}]`，对象里只能有 `id` |

新增记录请求体里的这些例子（文本、数字、单选、多选、日期、复选框、条码、人员、电话、超链接、附件、单向关联、双向关联、地理位置、货币、评分、进度、群组）来自[新增记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/create)。批量更新请求体给出同一组例子：[更新多条记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_update)。

## 明确不支持的类型

下面这些出现在字段元数据或数据结构文档里，但本程序拒绝写入：

- Formula（type 20）
- 查找引用（type 19；列出字段的 type 枚举未单列，数据结构文档有）
- CreatedTime（1001）、ModifiedTime（1002）
- CreatedUser（1003）、ModifiedUser（1004）
- AutoNumber（1005）
- 任何不在上一张表里的 `type` / `ui_type` 组合

拒绝发生在写接口之前。

## dry-run

命令：

```bash
python -m robot_pm.bitable apply path/to/edit.json --dry-run
```

dry-run 会取 token、列出字段、对 upsert 做搜索，然后在标准输出打印计划 JSON。它不调用新增、更新、删除，也不调用 `batch_create`、`batch_update`、`batch_delete`。

计划里 upsert 每一行有 `action`（`create` 或 `update`）、`key_field`、`key`、`record_id`。尚不存在的键没有 `record_id`。同一文件里前面的 upsert 已经计划创建该键时，后面的同一键标成 `update`，且 `depends_on_create` 为 true。

去掉 `--dry-run` 才会写。成功时退出码 0，标准输出是结果 JSON。失败时退出码 1，说明在标准错误。

## 部分批量失败

分三层。

1. 飞书一次 `batch_create` 或 `batch_update` 只有一个顶层 `code`。`code != 0` 时，本程序把这一次 HTTP 调用整批视为失败。官方响应没有逐条成功列表，因此不能判断这一批内部哪些行已经落库。见批量新增/更新文档的响应体：成功时 `code` 为 0 并返回 `records`，失败时是错误码表，不是逐条回执。
2. `batch_delete` 在 `code == 0` 时，`data.records[]` 仍可能有 `deleted: false`。文档：[删除多条记录](https://open.feishu.cn/document/server-docs/docs/bitable-v1/app-table-record/batch_delete)。`deleted` 不是 true，或请求了却没有回执的 record_id，算未删除。这时本程序报告已删除和未删除的 id，并停止后续操作。
3. 超过单次上限时，本程序自己分批。前一批已经成功、后一批失败，就是跨批次的部分成功。已经返回的 record_id 会放进错误结果；失败那一批 create/update 没有逐条回执。

编辑文件里的多个 operation 按顺序执行，没有事务。前面的操作已经写上的行不会回滚。某一个操作失败后，后面的操作不再执行。标准错误里的 JSON 含 `stopped: true` 和已经结束的操作；部分成功的那一步 `status` 为 `partial`。

dry-run 不产生上述写入，因此也没有部分写入。

## 其他错误

| 情况 | 行为 |
| --- | --- |
| 缺少环境变量 | 报缺失的变量名，不请求网络 |
| 取 token 失败或 HTTP 401 | 鉴权错误，不写 |
| 未知字段、不支持的类型、值的形状不对、编辑 JSON 不符合本文 | 整份文件不写 |
| upsert 键命中多条 | 该 upsert 不写；若前面的 operation 已经写过，那些写入保留 |
| `code` 为 1254290、1254291、1254112，或 HTTP 429 | 限流/写冲突。不自动重试。1254290 是请求过快，1254291 是同一数据表并发写冲突，1254112 出现在更新记录的错误码表里 |
| 其他非 0 `code` 或 HTTP 错误 | 接口错误，带上 `code` 和经过脱敏的 `msg`，然后停止 |

写接口按顺序调用，不对同一张表并发写。

## 命令与适配器

```bash
pip install -e ".[dev]"
python -m robot_pm.bitable apply examples/edit.example.json --dry-run
python -m robot_pm.bitable apply path/to/edit.json
pytest
```

示例编辑文件只用假字段名（`fake_item_code`、`fake_title`、`fake_count`、`fake_done`）和假 record id（`recEXAMPLE...`）。它不能对真实表执行，除非那张表恰好有这些字段。

测试只用 mock HTTP，不访问飞书。
