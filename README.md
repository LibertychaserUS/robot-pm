# Robot PM

飞书群里的项目助理。成员在群里 @ 机器人，或点卡片上的按钮。它按多维表格里的进度帮忙盯事情、记下职责，并在该开会时发一张卡片。会议要群里点同意才建立。时间用北京时间。

用法见 [用户手册](docs/user-manual.md)。

## 构建

需要 CMake 3.28 或更高，以及 GCC 14。持续集成用的就是这一组。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
ctest --test-dir build -L regression --output-on-failure
```

## 配置

凭证只放在运行环境里。仓库里的 [`.env.example`](.env.example) 只有占位符。不要提交真实的应用密钥、多维表格 token、群标识或日历标识。

飞书自建应用：

| 变量 | 作用 |
| --- | --- |
| `FEISHU_APP_ID` | 应用 ID |
| `FEISHU_APP_SECRET` | 应用密钥 |
| `FEISHU_ENCRYPT_KEY` | 事件加密 |
| `FEISHU_VERIFICATION_TOKEN` | 事件校验 |
| `FEISHU_BASE_URL` | 可选。默认 `https://open.feishu.cn` |

多维表格编辑器还要 `FEISHU_BITABLE_APP_TOKEN` 和 `FEISHU_BITABLE_TABLE_ID`。

## 多维表格编辑器

写入多维表格的小程序在 Python 里。字段名来自编辑文件，表在哪来自环境变量。

```bash
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/python -m robot_pm.bitable 写入 examples/edit.example.json --只检查
.venv/bin/pytest
```

`examples/edit.example.json` 里的字段都是假的，用来看文件形状。`--只检查` 只检查，不改表格。

## 程序用的提示

`prompts/` 里是机器人每一步用的系统提示。这些文件给程序读，不要在里面放密钥或真实的人名、群号。

## 部署前检查

持久化目录用一个 Cython 探针准备。它先检查主机，通过之后才建目录。检查失败就停，不留下半棵目录。它不启动 `robot-pm`，也不连接飞书。`apps/robot-pm/main.cpp` 目前只打印 `robot-pm`。

编译和运行见 [docs/deploy.md](docs/deploy.md)。
