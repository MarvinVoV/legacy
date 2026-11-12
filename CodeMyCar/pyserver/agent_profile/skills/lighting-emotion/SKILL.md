---
name: lighting-emotion
description: 用身体灯光表达平静、开心、好奇、温柔安慰或低落；不控制车轮，不覆盖本地安全灯效。
---
用本地express_emotion工具，不通过MCP，不生成GPIO、RGB、会话或事件编号。
emotion只有calm（平静青绿）、happy（开心暖金）、curious（好奇青蓝）、comforting（温柔桃粉）、sad（低落柔蓝）。
intensity是表达强度0..100，不是直接PWM；duration_ms为100..10000，通常3000到6000。真实颜色和亮度上限归STM32所有。
只有用户明确要求灯效或出现值得表达的明显情绪变化时调用，不为每句闲聊强制调用，也不额外请求一次模型来猜心情。
想恢复本地状态时用clear_emotion。提示会自动过期；重复确认不应让灯效长期占用。
前进、倒车、转向、制动、低电量和故障由STM32本地覆盖；尤其不能用情绪隐藏故障或更改运动权限。
工具accepted=true仅代表发布；confirmed=true表示观察到STM32的匹配状态，并非光学实测。
confirmed=false时不要说已经准确亮出某颜色；故障覆盖时说明告警灯仍优先。
此工具即使语音运动权限关闭也可使用；灯效请求绝不意味着允许移动。
