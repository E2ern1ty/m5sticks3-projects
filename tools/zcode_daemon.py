#!/usr/bin/env python3
"""ZCode 状态守护进程：读取 ~/.zcode/cli/db 的 sqlite，
在局域网 8765 端口提供 /status JSON，供 M5StickS3 仪表盘轮询。

用法: python3 zcode_daemon.py [--port 8765]
常驻运行，Ctrl+C 退出。只读方式打开数据库（不影响 ZCode 写入）。
"""
import json
import sqlite3
import time
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DB = str(Path.home() / ".zcode/cli/db/db.sqlite")


def read_status():
    now_ms = int(time.time() * 1000)
    out = {"state": "none", "title": "", "project": "", "ago": -1,
           "todos": {"done": 0, "total": 0, "current": ""},
           "tokens_total": 0, "tokens_out": 0, "cache_read": 0}
    try:
        db = sqlite3.connect(f"file:{DB}?mode=ro", uri=True, timeout=2)
        db.row_factory = sqlite3.Row
        row = db.execute(
            "SELECT id,title,directory,time_updated FROM session "
            "WHERE time_archived IS NULL AND task_type='interactive' "
            "ORDER BY time_updated DESC LIMIT 1").fetchone()
        if not row:
            db.close()
            return out
        sid = row["id"]
        out["title"] = (row["title"] or "")[:60]
        out["project"] = (row["directory"] or "").split("/")[-1][:20]
        out["ago"] = max(0, (now_ms - row["time_updated"]) // 1000)

        # todos 进度
        todos = db.execute(
            "SELECT content,status FROM todo WHERE session_id=? "
            "ORDER BY position", (sid,)).fetchall()
        done = sum(1 for t in todos if t["status"] == "completed")
        out["todos"]["total"] = len(todos)
        out["todos"]["done"] = done
        for t in todos:  # 当前：第一个 in_progress，否则第一个 pending
            if t["status"] == "in_progress":
                out["todos"]["current"] = t["content"][:40]
                break
        else:
            for t in todos:
                if t["status"] == "pending":
                    out["todos"]["current"] = t["content"][:40]
                    break

        # 最新 turn 状态
        turn = db.execute(
            "SELECT status,completed_at FROM turn_usage WHERE session_id=? "
            "ORDER BY started_at DESC LIMIT 1", (sid,)).fetchone()
        if turn and turn["status"] == "running":
            out["state"] = "running"
        elif turn and turn["status"] == "error":
            out["state"] = "error"
        else:
            out["state"] = "idle"  # 干完一段在等用户输入
        db.close()
    except sqlite3.Error as e:
        out["state"] = "none"
        out["title"] = f"db error: {e}"[:40]
    return out


def read_tokens():
    """全部会话累计 + 最近 24h 输出"""
    try:
        db = sqlite3.connect(f"file:{DB}?mode=ro", uri=True, timeout=2)
        row = db.execute(
            "SELECT COALESCE(SUM(computed_total_tokens),0), "
            "COALESCE(SUM(output_tokens),0), "
            "COALESCE(SUM(cache_read_input_tokens),0) FROM turn_usage").fetchone()
        day = db.execute(
            "SELECT COALESCE(SUM(output_tokens),0) FROM turn_usage "
            "WHERE started_at > ?", (int(time.time() * 1000) - 86400000,)).fetchone()
        db.close()
        return {"all_total": row[0], "all_out": row[1],
                "cache": row[2], "day_out": day[0]}
    except sqlite3.Error:
        return {"all_total": 0, "all_out": 0, "cache": 0, "day_out": 0}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/status"):
            st = read_status()
            st.update(read_tokens())
            body = json.dumps(st, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):  # 安静
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    import socket
    try:
        ip = socket.gethostbyname(socket.gethostname())
    except Exception:
        ip = "127.0.0.1"
    print(f"zcode daemon on http://{ip}:{args.port}/status")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
