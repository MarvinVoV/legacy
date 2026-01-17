from fastapi import HTTPException, Request
from fastapi.responses import JSONResponse


class AppException(Exception):
    """所有应用异常的基类"""

    def __init__(self, code: int = 500, message: str = "Internal Server Error", detail: dict = None):
        """
        初始化异常

        :param code: 错误码（默认 500）
        :param message: 用户友好的错误信息（默认 "Internal Server Error"）
        :param detail: 详细错误数据字典（默认空字典）
        """
        super().__init__(message)  # 确保兼容 Python 异常处理机制
        self.code = code
        self.message = message
        self.detail = detail or {}

    def __str__(self):
        """定义异常的字符串表示，包含所有关键信息"""
        return f"AppException(code={self.code}, message='{self.message}', detail={self.detail})"

    def to_dict(self) -> dict:
        """将异常转换为字典格式，便于序列化输出（如 API 响应）"""
        return {
            "code": self.code,
            "message": self.message,
            "detail": self.detail
        }


class CustomHTTPException(HTTPException):
    def __init__(self, status_code: int, code: int, message: str, detail: str = None):
        super().__init__(status_code=status_code, detail=detail)
        self.code = code
        self.message = message


async def custom_http_exception_handler(request: Request, exc: CustomHTTPException):
    return JSONResponse(
        status_code=exc.status_code,
        content={
            "code": exc.code,
            "message": exc.message,
            "detail": exc.detail
        },
    )


async def general_exception_handler(request: Request, exc: Exception):
    return JSONResponse(
        status_code=500,
        content={
            "code": 5000,
            "message": "Internal Server Error",
            "detail": str(exc)
        }
    )


async def app_exception_handler(request: Request, exc: AppException):
    return JSONResponse(
        status_code=500,
        content={
            "code": exc.code,
            "message": exc.message,
            "detail": str(exc.detail)
        }
    )


def register_exceptions(app):
    app.add_exception_handler(CustomHTTPException, custom_http_exception_handler)
    app.add_exception_handler(AppException, app_exception_handler)
    app.add_exception_handler(Exception, general_exception_handler)
