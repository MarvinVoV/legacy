FROM python:3.11-slim as builder
COPY requirements.txt .
RUN pip install --user -r requirements.txt

FROM python:3.11-slim as runtime
COPY --from=builder /root/.local /root/.local
COPY requirements .
RUN pip install -r requirements.txt