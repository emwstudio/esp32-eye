#!/bin/bash
# 防 Mac 睡眠地运行 esp32-eye 服务端
cd "$(dirname "$0")"
exec caffeinate -i .venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
