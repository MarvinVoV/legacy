# Agent Runtime Refactor Design (LangChain / Deep Agents)

**Status:** Implemented 2026-11-11. Phase 1 voice behavior remains unchanged; the LLM/agent layer is now isolated behind the new runtime.

**Goal:** replace the hand-written LLM history/tool loop with a layered LangChain/Deep Agents runtime that provides automatic context compression, persistent session state, memory-only self-evolution, and MCP tools, without coupling any of it to VAD/TTS/audio.

---

## 1. Why

The first conversation implementation put three concerns in one class:

- voice turn-taking (VAD, half-duplex, idle end);
- LLM transport and tool loop;
- conversation history/trimming.

That made memory, compression, MCP and session persistence hard to evolve. The refactor keeps the audio contract stable and moves the agent concerns into `app/agent`.

## 2. Layers

```text
app/api/routes/voice.py        WebSocket handshake / mode / lifecycle only
        |
app/services/conversation.py   VAD turn queue, half-duplex, sentence TTS, idle end
        |  AgentSession.stream(text)
app/agent/session.py            per-conversation LangGraph thread
app/agent/runtime.py            Deep Agent graph, checkpointer, tools, memory, MCP
app/agent/model.py              ChatDeepSeek factory
app/agent/memory.py             AGENTS.md / skills seed, filesystem permissions
app/agent/tools.py              car_* native tools + remember_preference
app/agent/mcp.py                MCPAdapter discovery/filter/lifecycle
        |
app/services/                  Baidu ASR/TTS, AgentMotionService, command mode
```

`ConversationRunner` still owns audio; it no longer owns history or a tool loop when an agent session is attached.

## 3. Deep Agents choice

We use `create_deep_agent` because the requested capabilities are built in:

- `MemoryMiddleware` for always-loaded `AGENTS.md`;
- `SkillsMiddleware` for progressive-disclosure `SKILL.md`;
- `SummarizationMiddleware` for automatic context compression;
- LangGraph checkpointer for persistent thread state;
- filesystem permissions for memory/skills boundaries;
- MCP tools passed alongside native tools.

LangGraph/Deep Agents were prototyped with `ChatDeepSeek`; first token stayed around 1 s. The per-session rebuild is intentional: it reloads `AGENTS.md` after a previous session updated it.

## 4. Session management

- Every voice session gets `thread_id = voice:{device_id}:{session_id}`.
- `AsyncSqliteSaver` stores messages, tool calls and summaries in `agent_data/sessions.sqlite`.
- A new wake starts a new thread; the checkpointer keeps the record for replay/inspection.
- Memory (`AGENTS.md`) persists across threads; agent sessions are rebuilt at start so updated memory is in the system prompt.
- Voice idle/end rules are unchanged: 700 ms turn pause, 15 s session idle, explicit end phrase.

## 5. Automatic compression

- `SummarizationMiddleware` is configured at **12,000 tokens / keep 8 messages**.
- Summaries are persisted by the checkpointer; recent messages stay intact.
- `AgentSession.stream()` filters `metadata["lc_source"] == "summarization"` so summaries are never spoken.
- Tool messages and filesystem tool outputs are also filtered; only `AIMessageChunk.text` reaches TTS.
- Compression happens inside the agent graph, so the audio layer does not manage token budget.

## 6. Memory-only self-evolution (approved boundary)

- Writable: `agent_data/AGENTS.md` (stable preferences, speaking style, car habits).
- Read-only: `agent_data/skills/**` (developer-approved procedures).
- The agent gets a dedicated `remember_preference` tool; it appends concise, deduplicated preference lines. It is not allowed to modify code, tools, or arbitrary files.
- `FilesystemPermission` rules deny writes to `/skills/**` and to every path except `/AGENTS.md`; the memory tool is the explicit write path.
- Per-session rebuild makes newly written memory effective in the next conversation.
- No autonomous code changes, no shell execution, no self-modifying pyserver.
- Secrets, raw audio and sensor values are explicitly out of memory scope.

## 7. MCP tools

- `langchain.mcp.MCPAdapter` (LangChain 1.4+) loads configured servers from `mcp.servers.json` (gitignored; `mcp.servers.example.json` is the template).
- Supports HTTP and stdio through the standard `mcpServers` shape.
- Tool allow/deny filters: `CMCAR_AGENT_MCP_ALLOW` / `CMCAR_AGENT_MCP_DENY`.
- MCP startup failure is non-fatal: voice and native car tools continue.
- MCP tool output is a `ToolMessage` and is never fed to TTS.
- The LangChain docs MCP server was used as an end-to-end probe: 3 tools discovered, and a real agent call answered a `create_agent` question.
- MCP is beta in LangChain 1.4; pin versions and test upgrades.

## 8. Native car tools

- `car_move`, `car_turn`, `car_stop` are LangChain tools using `ToolRuntime[AgentContext]`; device_id is injected per run.
- Execution remains behind `voice_execute_commands` and `AgentMotionService`/lease/reliable STOP.
- The tool result is the only source of truth for “executed”; the model is instructed not to claim success otherwise.
- One motion at a time; stop is always allowed. Phase 1 does not add a non-blocking move/stop design.

## 9. Streaming/TTS contract

- `AgentSession.stream()` yields model text chunks only.
- `ConversationRunner` sentence-splits and sends `speak` frames as before.
- Assistant playback drain still gates `assistant_done`; local playback is currently disabled (`CMCAR_VOICE_LOCAL_PLAYBACK=false`) so the development computer stays silent.
- The S3 receives PCM on the ES7210/ES8311 board and plays it (see
  `esp32_project/docs/voice/Audio-Board-Bring-Up-And-AEC-2026-11-12.md`);
  the local wake ack runs on-device and the server skips its ready tone
  when the start frame sets `local_ack`.

## 10. Configuration

```text
CMCAR_AGENT_ENABLED=1
CMCAR_AGENT_RUNTIME=deepagents        # or legacy
CMCAR_AGENT_DATA_DIR=agent_data
CMCAR_AGENT_CHECKPOINT_DB=agent_data/sessions.sqlite
CMCAR_AGENT_SUMMARY_TRIGGER_TOKENS=12000
CMCAR_AGENT_SUMMARY_KEEP_MESSAGES=8
CMCAR_AGENT_RECURSION_LIMIT=40
CMCAR_AGENT_MEMORY_TOOL_ENABLED=1
CMCAR_AGENT_MCP_ENABLED=1
CMCAR_AGENT_MCP_CONFIG=mcp.servers.json
CMCAR_AGENT_MCP_ALLOW=
CMCAR_AGENT_MCP_DENY=
```

Dependencies: `langchain[mcp]`, `langchain-deepseek`, `deepagents`, `langgraph-checkpoint-sqlite`, `aiosqlite`.

## 11. Rollback and command mode

- `CMCAR_AGENT_RUNTIME=legacy` or any agent-init failure falls back to the previous `ConversationRunner` LLM loop.
- Command mode (`mode=command`) never enters the agent runtime and remains unchanged.
- The old `app/services/llm.py` adapter remains as the legacy fallback.

## 12. Verification

- pyserver **210 tests passed**; ruff clean for the new modules.
- Real LangChain `ChatDeepSeek` streaming and tool calls verified.
- Real Deep Agents multi-turn conversation through the WebSocket: two turns, context retained.
- Memory tool probe: preference written to `AGENTS.md`, then read by a **new agent session**.
- MCP probe: `MCPAdapter` discovered 3 docs tools; a real agent call used the docs MCP server and answered.
- `agent_data/sessions.sqlite` created; checkpointer multi-turn state worked.
- No `afplay`/`ffplay` process; local playback false.
- S3 conversation firmware unchanged and still connects; real microphone/voice test remains for tomorrow.

## 13. Known limitations / next

- Barge-in is still phase 2; phase 1 is half-duplex.
- Device TTS output waits for ES7210 + DAC + NS4150B integration; host is silent.
- Session resume across reconnects is not exposed; new wake = new thread (checkpoints retained).
- Summarization has not been stress-tested with very long sessions; the 12k trigger is configurable.
- Memory is explicit/tool-driven, not a background curator; this is the approved safe boundary.
- MCP tool destruction policies and human approval for destructive MCP calls are future hardening.
- Deep Agents 0.7.x and MCP beta APIs must be pinned and regression-tested on upgrade.
