"""API 请求/响应模型"""

from pydantic import BaseModel


class CommonResponse(BaseModel):
    success: bool
    message: str = "ok"
    data: dict | list | None = None
