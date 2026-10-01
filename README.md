# robot PM

Learning Guide 的 side project。

飞书机器人是入口。云端 agent 规划项目进度，按需求和缺陷盯人、发进度报告，有问题就拉通相关人协调，并拉起会议。

## 多维表格编辑

当前代码是一个通用的飞书多维表格编辑程序：适配器调用开放平台，编辑文件描述要做的修改。表在哪、字段叫什么，都来自环境变量和编辑文件。

配置（只放占位符的示例在 `.env.example`，不要把真实密钥提交进仓库）：

- `FEISHU_APP_ID`
- `FEISHU_APP_SECRET`
- `FEISHU_BITABLE_APP_TOKEN`
- `FEISHU_BITABLE_TABLE_ID`
- `FEISHU_BASE_URL`（可选，默认 `https://open.feishu.cn`）

```bash
pip install -e ".[dev]"
python -m robot_pm.bitable apply examples/edit.example.json --dry-run
python -m robot_pm.bitable apply path/to/edit.json
pytest
```

编辑 JSON 的形状、字段值、dry-run、按键 upsert，以及鉴权、缺字段、类型、限流和部分批量失败，见 [docs/bitable-edit-spec.md](docs/bitable-edit-spec.md)。

这个编辑程序不包含盯人、拉会或进度报告。
