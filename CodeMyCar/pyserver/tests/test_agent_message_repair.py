from langchain_core.messages import AIMessage, HumanMessage, ToolMessage, RemoveMessage

from app.agent.middleware import IncompleteToolCallsMiddleware


def test_interrupted_tool_is_repaired_as_unknown_failure_without_replaying_action():
    call=AIMessage(content="",tool_calls=[{"id":"pending","name":"car_drive", "args":{"distance_cm":10}}])
    user=HumanMessage(content="先不要动")
    result=IncompleteToolCallsMiddleware().before_agent({"messages":[call,user]},None)
    messages=result["messages"]
    assert isinstance(messages[0],RemoveMessage)
    assert messages[1] is call and messages[-1] is user
    error=messages[2]
    assert isinstance(error,ToolMessage) and error.status=="error" and error.tool_call_id=="pending"
    assert "Do not assume success" in error.content
    assert IncompleteToolCallsMiddleware().before_agent({"messages":messages[1:]},None) is None


def test_complete_tool_result_is_preserved_and_malformed_call_is_closed():
    call=AIMessage(content="",tool_calls=[{"id":"done","name":"express_emotion","args":{"emotion":"happy"}}])
    result=ToolMessage(content='{"confirmed":true}',tool_call_id="done")
    assert IncompleteToolCallsMiddleware().before_agent({"messages":[call,result]},None) is None
    bad=AIMessage(content="",invalid_tool_calls=[{"id":"bad","name":"car_drive","args":"{","error":"truncated"}])
    repaired=IncompleteToolCallsMiddleware().before_agent({"messages":[bad]},None)["messages"]
    assert repaired[-1].tool_call_id=="bad" and repaired[-1].status=="error"
