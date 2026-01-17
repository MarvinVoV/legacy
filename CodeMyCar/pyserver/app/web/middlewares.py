import time

from fastapi import Request, Response
from app.core.logger import get_logger
from core.exception.exceptions import CustomHTTPException

logger = get_logger()


def register_middlewares(app):
    """集中注册所有中间件"""

    # 自定义中间件
    @app.middleware("http")
    async def log_requests(request: Request, call_next) -> Response:
        """请求日志中间件"""
        start_time = time.time()

        try:
            # 前置处理
            logger.info(
                "[Request] ",
                path=request.url.path,
                method=request.method
            )

            response = await call_next(request)

            # 后置处理
            process_time = time.time() - start_time
            response.headers["X-Process-Time"] = str(process_time)

            return response

        except CustomHTTPException as e:
            logger.error(
                "Request error",
                status_code=e.status_code,
                detail=e.detail
            )
            raise

        except Exception as e:
            logger.critical(
                "Unhandled exception",
                exc_info=str(e),
                stack=True
            )
            raise
