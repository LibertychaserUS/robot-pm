# 接入飞书

一个企业自建应用，打开机器人。凭证放在环境变量里，不进仓库：`FEISHU_APP_ID`、`FEISHU_APP_SECRET`、事件的 `FEISHU_ENCRYPT_KEY` 和 `FEISHU_VERIFICATION_TOKEN`。

飞书把事件发到 robot-pm 的地址。程序先验签。群消息没有 @ 机器人就丢掉。卡片按钮回调处理。私聊都处理。有人进群时，@ 这个人一次，请他 @ 机器人并阐述自己的职责。机器人入群时，发一条群消息，请大家各自 @ 机器人阐述职责，不逐个 @。

往外发用 `tenant_access_token`。消息走[发送消息](https://open.feishu.cn/document/server-docs/im-v1/message/create)，卡片的 `msg_type` 是 `interactive`。日程走[创建日程](https://open.feishu.cn/document/server-docs/calendar-v4/calendar-event/create)。多维表格走已有的 Python 编辑器。

文档不从飞书进。放在 `var/robot_pm/inbox/`，后台投影。
