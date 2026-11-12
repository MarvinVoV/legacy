from types import SimpleNamespace

import pytest

from app.services.conversation import ConversationConfig
from tests.test_voice_ws import FakeAsr, make_service, pcm, start_frame


def test_websocket_routes_to_exactly_one_agent_session(client,tmp_path):
    calls=[]
    class Session:
        async def stream(self,text):
            calls.append(text)
            yield "灯光表达好了。"
    class Runtime:
        async def aclose(self): pass
        def new_session(self,device,session_id):
            calls.append((device,session_id))
            return Session()
    client.app.state.voice_service=make_service(tmp_path,FakeAsr("开心一点"),tts_feedback=False)
    client.app.state.agent_runtime=Runtime()
    client.app.state.voice_conversation_config=ConversationConfig(idle_s=1,turn_end_ms=200)
    audio=pcm(.2)+pcm(.8,3000)+pcm(1)
    with client.websocket_connect("/api/v1/voice/stream") as ws:
        ws.send_text(start_frame(mode="conversation",local_ack=True))
        assert ws.receive_json()["type"]=="ready"
        ws.send_bytes(audio[:32000]);ws.send_bytes(audio[32000:])
        replies=[]
        while True:
            frame=ws.receive_json()
            if frame["type"]=="assistant_done":replies.append(frame["text"])
            if frame["type"]=="end":break
    assert replies==["灯光表达好了。"]
    assert len(calls)==2 and calls[0][0]=="DEV_1" and calls[1]=="开心一点"
