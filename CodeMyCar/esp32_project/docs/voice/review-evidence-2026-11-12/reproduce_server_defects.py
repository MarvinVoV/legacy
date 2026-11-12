"""Read-only defect reproductions for review baseline 600e08d.

Run with pyserver/.venv/bin/python. No cloud, socket, playback or car command.
These assertions describe current defects, not the desired fixed behavior.
"""

import asyncio
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "pyserver"))

from app.services.conversation import ConversationHistory
from tests.test_conversation import close_runner, make_runner


async def main():
    with tempfile.TemporaryDirectory(prefix="cmcar-voice-review-") as directory:
        service, session, runner, sink, *_ = await make_runner(Path(directory))
        runner.config = replace(runner.config, barge_in=True)
        runner.state = "speaking"
        await runner._begin_barge_in()
        print("after interrupt:", dict(
            post_stop=runner._post_stop,
            cooldown_active=runner._barge_cooldown_until > time.monotonic(),
            ignore_active=runner._ignore_until > time.monotonic(),
        ))
        assert not runner._post_stop
        assert runner._barge_cooldown_until == runner._ignore_until == 0

        def stop_count():
            return sum(e[0] == "json" and e[1].get("type") == "stop" for e in sink.events)

        before = stop_count()
        runner.device_barge_in()  # listening, not speaking; still accepted
        await asyncio.sleep(0.02)
        assert stop_count() == before + 1
        print("interrupt while listening accepted:", stop_count() - before)

        entered, release = asyncio.Event(), asyncio.Event()
        original_send = sink.send_json

        async def gated_send(payload):
            if payload.get("type") == "stop":
                entered.set()
                await release.wait()
            await original_send(payload)

        sink.send_json = gated_send
        runner.device_barge_in()
        await asyncio.wait_for(entered.wait(), 1)
        barges = [t for t in asyncio.all_tasks()
                  if t.get_coro().__qualname__ == "ConversationRunner._begin_barge_in"]
        await runner.close(abort=True)
        assert any(not t.done() for t in barges)
        print("untracked interrupt task survives runner.close:", True)
        release.set()
        await asyncio.gather(*barges)
        await close_runner(service, session, runner)

    history = ConversationHistory("system", max_turns=1, max_tokens=10000)
    history.add_user("u1")
    history.add_assistant("a1")
    history.add_user("u2")
    print("history after trim:", history.request_messages())
    assert history.request_messages()[1]["role"] == "assistant"
    print("All four defects reproduced against current imported production code.")


if __name__ == "__main__":
    asyncio.run(main())
