# 10 个开源项目：真实页面层级与交互调研

核对时间：2026-10-19。以下依据固定提交的路由、页面组件和模板源码整理，未安装运行十个项目；页面存在与控件由源码核实，视觉流畅度和真实触摸手感不在本次验证范围。弹窗、标签页和加载状态单列，避免把一个页面误写成多个独立 App。

最值得这块 4 岁儿童小板借鉴的是：Cozy 的大图块首页、HiKid 的角色/语音状态、KidsChat 的同页按住说话。小智、snflwr 与 Praeceptor 的管理功能主要供大人参考。

## OpenToys

源码版本：`e9827d76e55f`。核对入口：[页面/路由源码](https://github.com/akdeb/OpenToys/blob/e9827d76e55f9c1dc15c46e6b1af59d098fa562a/app/src/App.tsx)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 首次设置 /setup | 模型、语音、运行环境的准备步骤 | 完成后进入主界面；未完成由 SetupGate 拦截 |
| 体验首页 / 与 /playground | 人物/体验卡片、创建入口、设备连接状态、Play | 选体验→设备就绪→Play→会话 |
| 会话 /chat | 角色头像、语音状态、会话内容 | 结束后查看会话记录 |
| 历史 /conversations | 会话列表、历史文本 | 选会话→查看内容 |
| 声音 /voices | 声音卡片、预听、克隆入口 | 先预听或下载，再用于角色 |
| 用户 /users | 用户列表、新增/编辑弹窗 | 编辑完成回列表 |
| 设置 /settings | 模型/运行设置、连接 ESP32、刷机入口 | 设备操作放家长端 |
| 测试 /test | 开发测试页 | 不应暴露在儿童入口 |

借鉴：设备未就绪时明确禁用主按钮；选角色、模型和刷机由大人完成。小板只保留孩子真正要点的一个动作。

## xiaozhi-esp32-server

源码版本：`eb9dc8ca0fc8`。核对入口：[页面/路由源码](https://github.com/xinnan-tech/xiaozhi-esp32-server/blob/eb9dc8ca0fc8a30bc8afb0f302dcfc1578f76a7b/main/manager-web/src/router/index.js)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 入口 /、/home | 欢迎/首页、功能入口 | 进入设备或智能体管理 |
| 账号 /login、/register、/retrieve-password | 登录、注册、找回密码表单 | 登录守卫决定能否进后台 |
| 角色 /role-config；手机 agent/index、edit | 智能体列表、角色编辑、配置快照 | 选智能体→编辑→服务商/工具/语速音高 |
| 手机 agent/provider、tools、speedPitch | 服务商选择、工具配置、语速音高 | 保存后回角色编辑 |
| 设备 /device-management；手机 device/index | 设备列表、绑定/管理入口 | 设备→配置/会话历史 |
| 手机 device-config/index | Wi-Fi 选择和配置、超声波配置组件 | 选择 Wi-Fi→写入设备配置 |
| 手机 chat-history/index、detail | 历史列表、单次对话详情 | 点一条会话查看对话 |
| 声纹 /voice-print；手机 voiceprint/index | 声纹管理 | 从角色/设备管理进入 |
| 管理 /user-management、/model-config、/params-management | 用户、模型、参数页面 | 后台功能，归家长维护 |
| 知识与部署 /knowledge-base-management、/server-side-management、/ota-management | 知识库、服务端、固件管理 | 维护端管理入口 |
| 语音 /voice-resource-management、/voice-clone-management | 语音资源、克隆声音 | 配置后供智能体使用 |
| 系统 /dict-management、/provider-management、/feature-management、/replacement-word-management | 字典、供应商、功能开关、替换词 | 系统管理页 |
| 模板与通讯录 /agent-template-management、/template-quick-config、/address-book-management | 模板管理、快速配置、通讯录 | 角色配置的辅助入口 |
| 手机 settings/index、协议/隐私页 | 应用设置及法律说明 | 从手机登录/设置进入 |

借鉴：会话列表→单条详情的层级；大量后台配置不适合塞进 480 像素儿童屏。这里是设备管理系统，不是可直接照搬的儿童 UI。

## ai-child

源码版本：`4a5ada55b9e1`。核对入口：[页面/路由源码](https://github.com/ronaldowzy/ai-child/blob/4a5ada55b9e1bc9e4f1788aea47b5d0f7508ecff/android/app/src/main/java/com/childai/companion/ui/AppNavHost.kt)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| AuthScreen | 账号登录界面 | 未登录先验证；登录后默认 Chat |
| ChildChatScreen | 小白狐/房间、对话区、语音优先输入、拍照入口 | 说话→处理→角色回复；保持同一房间 |
| 家长入口 ParentEntryGate | 家长凭据验证 | 验证后进设置或报告 |
| ParentSettingsScreen | 称呼、年龄、兴趣、支持方式、目标、沟通偏好 | 保存/刷新/返回；也有退出登录 |
| ParentReportScreen | 家长报告 | 由儿童页的家长入口进入，返回原会话 |
| XiaozhantaiListScreen | 小展台列表 | 列表→作品详情 |
| XiaozhantaiDetailScreen | 已保存作品详情 | 查看后回小展台 |
| XiaozhantaiPickScreen | 为“奇怪的门”选作品 | 选中→回互动流程 |

借鉴：主场景稳定不频繁换页；角色状态告诉孩子现在该等、该说还是该听。年龄设置建议为 5–10 岁，不能当已适配 4 岁的证据。

## AshleyAI

源码版本：`5b375efa5639`。核对入口：[页面/路由源码](https://github.com/tkalevra/AshleyAI/blob/5b375efa5639158b1740acfca351733c1be8ad1f/frontend/pages/index.html)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| login.html | 用户登录 | 通过后到对话页 |
| index.html | 角色/人物区域、空状态、历史、对话输入 | 开始对话；历史从侧边入口打开 |
| 角色/会话弹窗 | 标题与可关闭内容区 | 在原页完成选择，不另开整页 |
| admin.html 家长验证 | 密码输入、家长设置门 | 验证后显示管理区 |
| People 标签 | 人物列表、Add someone | 添加/编辑家庭使用者 |
| Rules 标签 | 规则、模板、始终启用的规则 | 调整角色行为 |
| Model / Appearance / Password 标签 | 模型、外观、家长密码 | 同一管理页切标签 |
| help.html / about.html | 帮助与产品说明 | 独立辅助页 |

借鉴：管理页有明确家长门；大人编辑规则，儿童会话不显示模型字段。原始深色视觉不作为本板的配色参考。

## snflwr.ai

源码版本：`7feea47e4759`。核对入口：[页面/路由源码](https://github.com/snflwr-ai/snflwr.ai/blob/7feea47e475909134e92d359fe9e2916fd0cb451/api/static/dashboard/app.js)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 家长登录 views/login.js | 登录表单 | 登录守卫通过后显示控制台 |
| overview | 孩子数、会话数、待处理提醒、快捷卡片 | 卡片直达 Children / Activity / Safety |
| children | 孩子卡片、Add Child、编辑/停用/导出/删除 | 新增/编辑用弹窗；危险操作再次确认 |
| activity | 学习/使用活动视图 | 由总览或导航进入 |
| safety | 安全提醒、按孩子查看事件、确认已读 | 提醒→对应孩子事件 |
| settings | 账号、通知邮箱、密码、说明、退出 | 家长维护账号 |
| 独立 admin/index.html | 管理员控制台 | 独立于家长 dashboard |

借鉴：需要处理的事情可以从首页直接进详情。该仓库儿童聊天依赖另一套界面；本次核对的是自带家长控制台，不能虚构它有一套独立儿童聊天页。

## praeceptor

源码版本：`6514ef3a5780`。核对入口：[页面/路由源码](https://github.com/studylamp/praeceptor/blob/6514ef3a5780a248e6dcbd7000a8391119453a53/app/templates/student/subjects.html)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| student/login.html | 学生登录 | 登录后显示已授权科目 |
| student/subjects.html | 科目入口 | 点科目→该科目聊天 |
| student/chat.html | 消息、输入区、历史分段加载 | 发送→回复；可加载更早记录 |
| student/help.html | 学生帮助 | 从学生端进入 |
| admin/login.html | 家长/管理员登录 | 通过后进 Students |
| admin/dashboard.html | 学生列表、Add student、Manage | 学生→详情 |
| admin/student_form.html、student_detail.html | 学生资料、限额、PIN、科目、会话 | 保存、重置、添加科目；删除进入确认页 |
| admin/subject_form.html、subject_context.html、subject_copy.html、subject_prompts.html | 科目编辑、教材背景、复制、实际提示词 | 从学生详情进入；返回原学生 |
| admin/curriculum.html | 按学生汇总科目背景 | 点 Update→编辑科目背景 |
| admin/conversations.html、conversation.html | 全部会话列表、单次对话 | 列表→阅读→可加载全部/删除确认 |
| admin/settings.html、test_chat.html、confirm_delete.html | 全局设置、家长测试对话、删除确认 | 维护与危险动作分开 |

借鉴：儿童只看自己的入口；家长会话列表→详情。它偏文字家教，不适合直接复制给 4 岁儿童。

## kidschat

源码版本：`3bf58e8d3062`。核对入口：[页面/路由源码](https://github.com/rwightman/kidschat/blob/3bf58e8d3062b7b141f5158ac9645feccced7266/frontend/templates/index.html)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 单一聊天主页 | 顶部连接状态、中间消息、底部麦克风/拍照/文本输入、右侧 Talking Head | 从头到尾停留同一页 |
| 欢迎空状态 | 带 emoji 的示例问题按钮 | 点一个直接填入/发起对应话题 |
| 语音输入状态 | Hold to talk 麦克风 | 按住说话；状态用按钮变化反馈 |
| 相机弹层 camera-modal | 实时预览、问题输入、Capture | 拍照后出现 Retake 与 Ask About Picture |
| 相片确认态 | 已拍图片、重拍、提问 | 重拍回预览；确认后回聊天 |

借鉴：一个稳定主屏、麦克风状态明确；底部按钮原样搬到小屏会太密，应只保留主要动作。源码没有独立的家长报告/设置路由。

## hi-kid

源码版本：`e3c78992743c`。核对入口：[页面/路由源码](https://github.com/xiaochong/hi-kid/blob/e3c78992743cd4d2934fa6bad59c12ca5ac7a325/src/renderer/src/App.tsx)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| loading | 启动/检查状态 | 检查后转依赖准备、下载或会话 |
| deps-setup | 依赖准备步骤 | 完成后继续下载/初始化 |
| download | 模型/资源下载和进度 | 完成后进入引导或会话 |
| onboarding | 初次引导 | 完成后进入 conversation |
| conversation | 卡通角色、主语音按钮、可选文本侧栏 | 按住或 VAD 模式→聆听/思考/说话状态 |
| IdeasMenu | 话题与小游戏入口 | 选话题/开始游戏后收起菜单 |
| SettingsPanel / TextToggle | 语音模式、设置、文本开关 | 在会话原页开关，不强迫孩子阅读聊天记录 |

借鉴：默认围绕角色与说话按钮；文字记录可以隐藏。模型下载和环境准备由家长完成后，儿童无需反复经过。

## kid-english-reading

源码版本：`e0b0c093af1e`。核对入口：[页面/路由源码](https://github.com/qqhhhh/kid-english-reading/blob/e0b0c093af1eaf28b4c989b4d966ffd2e9498991/src/App.tsx)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 默认儿童练习页 | 当前课文、听读、录音、练习状态与反馈 | 课文→练习→语音评估→查看反馈 |
| /login | ParentAccessGate 登录 | 通过后进入受保护功能 |
| /parent | 家长控制台 | 管理练习/孩子设备；与儿童页分开 |
| /admin | 平台管理 | 平台管理员维护 |
| /local-course-studio | 本地课程工作台 | 大人制作课程内容 |
| /sudoku | 数独页 | 独立游戏路由 |
| /picture-books | 绘本广场 | 绘本选择入口 |
| /picture-book-preview | 绘本原型预览 | 查看绘本交互 |
| /preview | 设备/主题预览工作台 | 大人检查屏幕适配 |
| /filing-review | 审核演示 | 独立演示入口 |

借鉴：练习按钮用状态球，成功/问题在当前位置反馈；设备预览工具有助于避免每次刷机才能看布局。读英语与评估细节不宜塞进当前家庭语音页。

## cozy-kids-launcher

源码版本：`91b71573155a`。核对入口：[页面/路由源码](https://github.com/TrissyGE/cozy-kids-launcher/blob/91b71573155a8a753ab9563f348e2e0fe612b0b2/src/index.html)。

| 页面或状态 | 看见什么 | 操作后到哪里 |
|---|---|---|
| 儿童主页 | 全屏大图块、分页、返回/导航 | 点图块启动应用或媒体；支持大/小布局 |
| profilePicker | 孩子头像/身份选择 | 选择后进入该孩子的主页 |
| firstRun | 首次运行引导 | 配置后进入可用首页 |
| 本地媒体库 | 封面、收藏、最近播放 | 选内容→播放器；保留恢复播放状态 |
| PIN 弹层 | 家长 PIN | 验证后进入管理或解除时间限制 |
| 家长 Overview / Children / Apps | 总览卡片、孩子资料、应用图块编辑 | 从总览卡片直达对应管理标签 |
| 家长 Screen Time | 时间与计划管理 | 到时显示警告→阻断屏→家长 PIN |
| 家长 Appearance / Theme | 主题、颜色、大字、高对比、减少动态、实时预览 | 配置预览后保存 |
| 家长 System / Backup | 系统维护、备份恢复 | 留在家长管理范围 |
| 启动/不可用/更新状态层 | 正在启动、当前不可用、更新进度 | 在主屏上覆盖状态；结束后回主屏 |

借鉴：4 岁孩子最适合学这一套“大图块首页”；同时可以保留大字、高对比、减少动态。家长设置不用跟儿童功能抢屏幕。

## 落到本项目的页面

首页只有「摇骰子」「说说话」两张大卡片，设置收在底部。说说话直接进家庭对话：无来信时大面积按住说话；有来信时上方一个大重听区、下方一个大说话区。按下整屏变色，松开发送；成功用对勾，随后回原屏。来信自动进入播放，播完回同一屏。

字体使用简体中文 Medium，32/40 原生像素，4 位灰阶边缘；不再使用字库默认的日文 Thin 字体或将小字粗暴放大。

证据索引见 `research/ui-source-manifest.json`，列出固定提交及实际取得的界面源文件；没有下载、安装或执行这些项目的依赖。
