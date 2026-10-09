# Robot PM

飞书群里的项目助理。成员在群里 @ 机器人，或点卡片上的按钮。它按多维表格里的进度帮忙盯事情、记下职责，并在该开会时发一张卡片。会议要群里点同意才建立。时间用北京时间。

用法见 [用户手册](docs/user-manual.md)。

## 准备时记下什么

凭证放在运行环境里。仓库里的 [`.env.example`](.env.example) 只有占位符。不要提交真实的应用密钥、多维表格 token、群标识或日历标识。

飞书这几项要有值。程序缺了会打出左边的名字。

| 名字 | 变量 |
| --- | --- |
| 应用编号 | `FEISHU_APP_ID` |
| 应用密钥 | `FEISHU_APP_SECRET` |
| 加密密钥 | `FEISHU_ENCRYPT_KEY` |
| 校验口令 | `FEISHU_VERIFICATION_TOKEN` |

`FEISHU_BOT_OPEN_ID` 不设就认不出群里的点名。

部署探针还要 `FEISHU_BITABLE_APP_TOKEN` 和 `FEISHU_BITABLE_TABLE_ID` 有值。它只检查在不在。缺了就在标准错误里写出变量名。

可以不设的有 `FEISHU_BASE_URL`（默认 `https://open.feishu.cn`）、`FEISHU_GROUP_ID`、`FEISHU_CALENDAR_ID`、`ROBOT_PM_TIMEZONE`（默认 `Asia/Shanghai`）、`ROBOT_PM_PORT`（默认 `8080`）。

每个群的形式要事先记下。现在还没有写入这一步。没记下就不会发决定。形式是普通群或话题群。

数据目录用 `ROBOT_PM_DATA_ROOT`。不设就是当前目录下的 `var/robot_pm`。探针和程序要用同一个目录。

## 部署探针

探针检查这台机器能不能放数据目录。通过之后才把目录建出来。它不启动 `robot-pm`，不监听端口，也不连接飞书。

需要 Python 3.11 或更高。探针还要求 GCC 14 或 Clang 18，以及 CMake 3.28 或更高。

```bash
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/robot-pm-deploy --目录 var/robot_pm
```

`--目录` 省略时用 `ROBOT_PM_DATA_ROOT`，再省略就是 `var/robot_pm`。

它检查这些。任何一步失败，目录保持原样。原因写在标准错误上，退出码是 1。环境变量的值不会打印。

1. 这六项都有非空的值：`FEISHU_APP_ID`、`FEISHU_APP_SECRET`、`FEISHU_ENCRYPT_KEY`、`FEISHU_VERIFICATION_TOKEN`、`FEISHU_BITABLE_APP_TOKEN`、`FEISHU_BITABLE_TABLE_ID`。
2. 时钟能换成北京时间（`Asia/Shanghai`，东八区），年份落在 2026 到 2100 之间。
3. 当前解释器和 `python3` 都是 3.11 或更高，`cmake` 至少 3.28，C++ 编译器是 GCC 14 或 Clang 18。
4. 可用空间至少 **50466816 字节**。不够就停，不会先建目录。
5. 按这个字节数写一份探针文件并 `fsync`，然后删掉。写失败就停。
6. 同一时间只能有一个写入者。锁文件是 `.writer.lock`。已经有进程占着，就停。

成功时打印目录路径，以及「至少需要 50466816 字节」。

`--占着` 会在目录就绪后一直占着这把锁，直到标准输入结束。用来确认第二个进程不能同时准备这棵树。它仍然不启动机器人。平常跑完，锁会放开。`robot-pm` 处理事件时不拿这把锁。

空间怎么分成三笔，见 [docs/deploy.md](docs/deploy.md)。

## 让它一直开着

先编译。需要 CMake 3.28 或更高，以及 GCC 14。持续集成用的就是这一组。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
ctest --test-dir build -L regression --output-on-failure
```

在仓库根目录启动，这样找得到 `prompts/`。数据目录用上面记下的那个。

```bash
export ROBOT_PM_DATA_ROOT=var/robot_pm
./build/apps/robot-pm/robot-pm
```

进程听着端口，一直处理飞书事件，直到收到 SIGINT 或 SIGTERM。它不会只打印 `robot-pm` 就退出。

缺了应用编号、应用密钥、加密密钥或校验口令，它把缺的名字一行一个打出来，然后退出，退出码是 1。已经有值的项不会打出来。

端口写错会打印「端口不对」并退出。端口被占用会打印「端口被占」并退出。默认端口是 8080。

跑起 `robot-pm`，进程就一直留着。这次改动没有带上用户态守护。

## 发出去的卡片

提出人的按钮是 **确认** 和 **取消**。开会的按钮是 **同意** 和 **先不办**。

已经记下普通群，并且群就是普通群：该点的人得到一张群里的卡片，只有本人能看见。普通群里消息是帖子时，也是这一张。

已经记下话题群，并且群就是话题群：同一个人在私聊里得到同一条决定。

没有记下形式，或者形式和群对不上：什么都不发。

这些卡片不是部署探针发的。

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
