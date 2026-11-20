"""LAN demo inspection service. Python standard library only; no cloud dependency."""
import argparse
from contextlib import closing
import hashlib
import hmac
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import secrets
import sqlite3
import time
from urllib.parse import parse_qs, urlsplit
from datetime import datetime, timezone

DEFAULT_DEVICE = "HDPAP00008-325JPC4X0I"

class InspectionApp:
    def __init__(self, database, device=DEFAULT_DEVICE):
        self.database = str(database)
        self.device = device
        self.secret = secrets.token_bytes(32)
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("""CREATE TABLE IF NOT EXISTS inspections (
                id INTEGER PRIMARY KEY, created_utc TEXT NOT NULL,
                device TEXT NOT NULL, inspector TEXT NOT NULL,
                result TEXT NOT NULL, note TEXT NOT NULL, token TEXT UNIQUE NOT NULL)""")

    def token(self):
        message = str(int(time.time())) + "." + secrets.token_hex(16)
        signature = hmac.new(self.secret, (self.device + message).encode(), hashlib.sha256).hexdigest()
        return message + "." + signature

    def valid_token(self, token):
        try:
            timestamp, nonce, signature = token.split(".")
            age = time.time() - int(timestamp)
            expected = hmac.new(self.secret, (self.device + timestamp + "." + nonce).encode(),
                                hashlib.sha256).hexdigest()
            return 0 <= age < 900 and len(nonce) == 32 and hmac.compare_digest(signature, expected)
        except (ValueError, TypeError):
            return False

    def save(self, fields):
        expected = {"device", "inspector", "result", "note", "token"}
        if set(fields) != expected or any(len(value) != 1 for value in fields.values()):
            raise ValueError("提交字段不完整，请重新打开巡检页面。")
        values = {key: value[0].strip() for key, value in fields.items()}
        if values["device"] != self.device or not self.valid_token(values["token"]):
            raise ValueError("设备编号或页面凭证无效，请重新打开页面。")
        if not 1 <= len(values["inspector"]) <= 40 or len(values["note"]) > 500:
            raise ValueError("巡检人须为 1–40 字，备注最多 500 字。")
        if values["result"] not in ("normal", "abnormal"):
            raise ValueError("请选择有效的巡检结果。")
        with closing(sqlite3.connect(self.database, timeout=10)) as db, db:
            db.execute("""INSERT OR IGNORE INTO inspections
                (created_utc, device, inspector, result, note, token) VALUES (?, ?, ?, ?, ?, ?)""",
                (datetime.now(timezone.utc).isoformat(), values["device"], values["inspector"],
                 values["result"], values["note"], values["token"]))
            return db.execute("SELECT id FROM inspections WHERE token=?", (values["token"],)).fetchone()[0]

    def handler(self):
        app = self
        class Handler(BaseHTTPRequestHandler):
            def page(self, code, body):
                document = """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>校园应急物资站 · 巡检</title>
<style>body{font:17px/1.6 system-ui,sans-serif;background:#f0f4f7;color:#182b39;margin:0}
main{max-width:520px;margin:32px auto;padding:24px;background:white;border-radius:16px}
h1{font-size:25px}label{display:block;margin:18px 0 6px}input,select,textarea,button{
box-sizing:border-box;width:100%;padding:12px;font:inherit;border:1px solid #aabac5;border-radius:8px}
button{background:#075f69;color:white;border:0;margin-top:20px}small{color:#526471}
@media(max-width:580px){main{margin:12px;padding:20px}}</style><main>""" + body + "</main></html>"
                payload = document.encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Content-Security-Policy",
                                 "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                url = urlsplit(self.path)
                query = parse_qs(url.query, keep_blank_values=True)
                if url.path != "/inspect" or query != {"device": [app.device]}:
                    self.page(404, "<h1>未找到巡检设备</h1><p>请使用该设备标签中的完整链接。</p>")
                    return
                device = html.escape(app.device, quote=True)
                token = app.token()
                self.page(200, f"""<h1>校园应急物资站巡检</h1><p>设备：<strong>{device}</strong></p>
<small>请现场检查物资、外观和设备状态，确认后提交。仅打开此页不会产生巡检记录。</small>
<form method="post" action="/inspect">
<input type="hidden" name="device" value="{device}"><input type="hidden" name="token" value="{token}">
<label for="inspector">巡检人</label><input id="inspector" name="inspector" maxlength="40" required autocomplete="name">
<label for="result">巡检结果</label><select id="result" name="result" required>
<option value="">请选择</option><option value="normal">正常</option><option value="abnormal">异常</option></select>
<label for="note">备注（选填）</label><textarea id="note" name="note" maxlength="500" rows="3"></textarea>
<button type="submit">确认并保存巡检记录</button></form>""")

            def do_POST(self):
                if self.path != "/inspect":
                    self.page(404, "<h1>路径不存在</h1>"); return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                    if not 0 < length <= 8192 or self.headers.get("Transfer-Encoding"):
                        raise ValueError("请求大小不正确。")
                    if self.headers.get("Content-Type", "").split(";")[0] != "application/x-www-form-urlencoded":
                        raise ValueError("请求格式不正确。")
                    self.connection.settimeout(5)
                    raw = self.rfile.read(length)
                    if len(raw) != length:
                        raise ValueError("请求不完整。")
                    fields = parse_qs(raw.decode("utf-8"), keep_blank_values=True, max_num_fields=8)
                    record_id = app.save(fields)
                except (ValueError, UnicodeError, TimeoutError) as exc:
                    self.page(400, "<h1>未保存</h1><p>" + html.escape(str(exc)) + "</p>")
                    return
                except sqlite3.Error:
                    self.page(500, "<h1>保存失败</h1><p>请联系管理员检查数据库。</p>")
                    return
                self.page(200, f"<h1>巡检已记录</h1><p>记录编号：{record_id}</p>"
                          "<p>重复提交此页面不会增加相同记录。可以关闭页面。</p>")
        return Handler

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--db", default=str(Path(__file__).with_name("inspections.sqlite3")))
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    args = parser.parse_args()
    app = InspectionApp(args.db, args.device)
    server = ThreadingHTTPServer((args.host, args.port), app.handler())
    print(f"Inspection service: http://{args.host}:{args.port}/inspect?device={args.device}", flush=True)
    print(f"SQLite: {app.database} | LAN demo only, no login/authentication.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
