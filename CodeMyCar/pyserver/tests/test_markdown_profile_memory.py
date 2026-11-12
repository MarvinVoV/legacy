"""Markdown source of truth, atomic updates/migration, session reload and identity."""

import shutil
import sqlite3
from concurrent.futures import ThreadPoolExecutor

import pytest
from langchain_core.messages import AIMessage

from app.agent.persona import DEFAULT_PROFILE_DIR, ProfileError, load_profile
from app.agent.preferences import MemoryFormatError, Preference, PreferenceStore
from tests.test_companion_graph import make_runtime


def test_no_authored_prompt_in_python_and_profile_validation(tmp_path):
    shutil.copytree(DEFAULT_PROFILE_DIR, tmp_path / "profile")
    profile = tmp_path / "profile"
    assert "小鸭" in load_profile(profile).system_prompt
    (profile / "ROLE.md").write_text("")
    with pytest.raises(ProfileError):
        load_profile(profile)
    (profile / "ROLE.md").write_text("你叫小鸭")
    (profile / "prompts" / "SUMMARY.md").write_text("wrong {variable}")
    with pytest.raises(ProfileError):
        load_profile(profile)


def test_manual_notes_survive_tools_and_human_edits_reload(tmp_path):
    store = PreferenceStore(tmp_path / "memories")
    scope = store.scope("DEV_1", "household")
    original = store.read_document(scope)
    path = store.document_path(scope)
    path.write_text(original.replace("## 手动记录", "## 手动记录\n\n小狗叫豆包。"))
    item = Preference(category="fact", key="cat_name", value="家里的小猫叫团团")
    store.remember(scope, item)
    assert "小狗叫豆包。" in path.read_text()
    assert "- [fact/cat_name] 家里的小猫叫团团" in path.read_text()
    path.write_text(path.read_text().replace("团团", "圆圆"))
    assert store.recall(scope)[0]["value"].endswith("圆圆")
    store.forget(scope, "fact", "cat_name")
    assert "圆圆" not in path.read_text() and "小狗叫豆包。" in path.read_text()


def test_concurrent_atomic_updates_no_lost_memories(tmp_path):
    store = PreferenceStore(tmp_path / "memories", max_entries=100)

    def update(index):
        # Independent store objects mimic separate request owners/process opens.
        other = PreferenceStore(store.path, max_entries=100)
        other.remember("scope", Preference(category="fact", key=f"fact_{index}", value=str(index)))

    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(update, range(30)))
    assert len(store.recall("scope")) == 30


def test_failed_atomic_replace_preserves_original_and_removes_temp(tmp_path, monkeypatch):
    store = PreferenceStore(tmp_path / "m")
    original = store.read_document("scope")

    def fail(*args):
        raise OSError("simulated full disk")

    monkeypatch.setattr("app.agent.preferences.os.replace", fail)
    with pytest.raises(OSError):
        store.remember("scope", Preference(category="fact", key="cat", value="团团"))
    path = store.document_path("scope")
    assert path.read_text() == original
    assert sorted(p.name for p in path.parent.iterdir()) == [".memory.lock", "MEMORY.md"]


def test_malformed_or_symlink_memory_is_not_overwritten(tmp_path):
    store = PreferenceStore(tmp_path / "m")
    store.read_document("scope")
    path = store.document_path("scope")
    path.write_text("invalid manual format")
    with pytest.raises(MemoryFormatError):
        store.remember("scope", Preference(category="fact", key="cat", value="团团"))
    assert path.read_text() == "invalid manual format"
    path.unlink()
    target = tmp_path / "outside.md"
    target.write_text("outside")
    path.symlink_to(target)
    with pytest.raises(MemoryFormatError):
        store.read_document("scope")
    assert target.read_text() == "outside"


def test_old_sqlite_migrates_once_per_scope_never_resurrects_deleted_memory(tmp_path):
    db_path = tmp_path / "preferences.sqlite"
    scope = PreferenceStore.scope("DEV_1", "household")
    with sqlite3.connect(db_path) as db:
        db.execute("CREATE TABLE preferences(scope,category,key,value,updated)")
        db.execute(
            "INSERT INTO preferences VALUES (?,?,?,?,?)", (scope, "interest", "animal", "猫", 1)
        )
        db.execute(
            "INSERT INTO preferences VALUES (?,?,?,?,?)", ("other", "interest", "animal", "狗", 2)
        )
    store = PreferenceStore(tmp_path / "memories", legacy_db=db_path)
    assert store.recall(scope) == [{"category": "interest", "key": "animal", "value": "猫"}]
    store.forget(scope, "interest", "animal")
    assert not PreferenceStore(store.path, legacy_db=db_path).recall(scope)
    assert db_path.is_file()


async def test_new_sessions_reload_role_and_read_updated_markdown(tmp_path):
    profile = tmp_path / "profile"
    shutil.copytree(DEFAULT_PROFILE_DIR, profile)
    runtime, _ = make_runtime(
        tmp_path / "runtime", [AIMessage(content="你好。")], profile_dir=profile
    )
    first = runtime.new_session("DEV_1", "first")
    role = profile / "ROLE.md"
    role.write_text(role.read_text() + "\n你喜欢把问题变成一个小小的探险。\n")
    second = runtime.new_session("DEV_1", "second")
    assert first.agent is not second.agent
    assert "小小的探险" in runtime.profile.system_prompt
    scope = runtime.preferences.scope("DEV_1", "household")
    runtime.preferences.remember(
        scope, Preference(category="fact", key="cat_name", value="小猫叫团团")
    )
    runtime.model.responses = [AIMessage(content="团团。")]
    assert "".join([t async for t in second.stream("你记得猫叫什么吗")]) == "团团。"
    assert "团团" in str(runtime.model.seen[-1][0].content)
    path = runtime.preferences.document_path(scope)
    path.write_text(path.read_text().replace("团团", "圆圆"))
    runtime.model.responses = [AIMessage(content="圆圆。")]
    third = runtime.new_session("DEV_1", "third")
    assert "".join([t async for t in third.stream("猫现在叫什么")]) == "圆圆。"
    assert "圆圆" in str(runtime.model.seen[-1][0].content)
