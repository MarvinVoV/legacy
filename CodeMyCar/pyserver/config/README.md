# 模型与语音厂商配置

`providers.toml`是公开、可提交的厂商配置。实际密钥只放pyserver/.env、voice.local.env或进程环境；例如：

```dotenv
CMCAR_LLM_API_KEY=填写自己的值
CMCAR_BAIDU_SPEECH_API_KEY=填写自己的值
CMCAR_BAIDU_SPEECH_SECRET_KEY=填写自己的值
MY_SPEECH_API_KEY=填写自己的值
```

这两个.env文件已经被Git忽略。TOML里只写`api_key = "${MY_SPEECH_API_KEY}"`，代码拒绝明文密钥与非空密钥默认值。普通选项支持`${VAR:-default}`；缺失的可选厂商密钥禁用相应客户端，不使用空密钥发送请求。URL、模型和音色都在TOML中维护。

新增一个兼容厂商：

```toml
[providers.my_speech]
kind = "openai-compatible"
base_url = "https://your-provider.example/v1"
api_key = "${MY_SPEECH_API_KEY}"
```

再将已有`[asr]`或`[tts]`中的provider改成`"my_speech"`，填写其支持的model、voice等；**编辑已有区块，不重复添加同名区块**。ASR与TTS可以选择不同厂商。改厂商配置后重启pyserver。

已有协议适配：

- LLM：deepseek、openai-compatible（Chat Completions/tool calling）。DeepSeek thinking扩展不会发给普通OpenAI兼容服务。
- ASR/TTS：baidu、openai-compatible。后者需支持`/audio/transcriptions`文件上传与`/audio/speech`的WAV输出；“LLM接口兼容”不意味着这个厂商也提供兼容语音接口。
- 百度OAuth、ASR、TTS URL独立配置；原103/语速6/音调5保持。新语音适配器统一转换为16kHz/单声道/PCM16，音频会话和S3不用认识厂商差异。

同协议厂商仅改配置；不同鉴权/协议仍需添加Python适配器和schema，不能靠换URL实现。当前服务实际仍用百度；新兼容语音适配器经过HTTP模拟测试，未宣称已在任意第三方厂商实机验收。

配置优先级：TOML字面值直接生效；`${VAR}`依次从.env、voice.local.env、进程环境读取，后者优先。程序显式传入的旧Settings字段遵从Pydantic优先级。只有显式提供的兼容字段才参与插值，不让旧Settings默认值压住TOML默认值。原有环境变量仍兼容，不会改写现有本地.env。

默认文件路径与源码位置绑定；可用`CMCAR_PROVIDERS_CONFIG`选另一份TOML，`CMCAR_AGENT_PROFILE_DIR`选另一套Markdown角色。相对配置路径相对启动工作目录，推荐从pyserver启动。

```bash
.venv/bin/python -m scripts.check_configuration
```

会校验版本、provider选择、URL、参数范围和Markdown，不输出密钥；不向云端发请求。凭证缺失时观察credentials_available=false。控制限幅、语音执行开关、服务器端口/MQTT等运行参数仍由Settings与.env管理，不属于厂商适配协议。

固件的本地“我在”是静态PCM资源，不会随服务端配置自动改变。若要同步更换，显式运行`../esp32_project/tools/make_wake_ack.py`并重新构建/烧录；该工具现在使用同一份TTS配置，资源生成会调用所选厂商并产生费用。本轮没有重新生成或烧录。
