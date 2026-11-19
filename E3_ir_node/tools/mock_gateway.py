#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""应急物资站 · 假网关（PC 端联调工具）

作用：主控板还没写好、或者手上只有一块红外板的时候，先在电脑上扮演网关，
      看红外节点到底发没发、发的是什么、状态变了几次。

用法：
    python tools/mock_gateway.py                     # 只收，监听 8888
    python tools/mock_gateway.py --port 8888
    python tools/mock_gateway.py --node 192.168.43.50
        # 收的同时可以手动给红外板发命令：直接回车 = CMD=IR,PING=1
    python tools/mock_gateway.py --node 192.168.43.50 --send "CMD=IR,PING=1"
        # 只发一条就退出

前提：电脑和开发板连同一个 Wi-Fi。Windows 第一次运行会弹防火墙窗口，点"允许"。
"""

import argparse
import os
import socket
import sys
import threading
import time


def parse(payload):
    """NODE=IR,P=1  ->  {'NODE': 'IR', 'P': '1'}"""
    fields = {}
    for part in payload.split(","):
        part = part.strip()
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key.strip().upper()] = value.strip()
    return fields


def fmt_report(text):
    """给收到的报文补一句人话，方便扫一眼串口输出。"""
    fields = parse(text)
    if text.startswith("NODE="):
        if fields.get("NODE") == "IR":
            p = fields.get("P", "?")
            return "<= 红外: %s" % ("有人 P=1" if p == "1" else "无人 P=0" if p == "0" else "P=" + p)
        return "<= 节点 %s 上报" % fields.get("NODE", "?")
    if text.startswith("CMD="):
        return "<= 命令 %s" % fields.get("CMD", "?")
    return ""


def input_loop(sock, node, port):
    print("[GW] 回车 = 发 CMD=IR,PING=1 ；输入其它内容则原样发；输入 q 退出", flush=True)
    while True:
        try:
            line = input()
        except EOFError:
            return
        line = line.strip()
        if line.lower() in ("q", "quit", "exit"):
            print("[GW] bye", flush=True)
            os._exit(0)
        cmd = line if line else "CMD=IR,PING=1"
        sock.sendto(cmd.encode("utf-8"), (node, port))
        print("[GW] -> %s:%d  %s" % (node, port, cmd), flush=True)


def main():
    ap = argparse.ArgumentParser(description="应急物资站 · 假网关（收节点 UDP 上报）")
    ap.add_argument("--bind", default="0.0.0.0", help="监听地址，默认 0.0.0.0")
    ap.add_argument("--port", type=int, default=8888, help="监听端口，默认 8888（主控收数据口）")
    ap.add_argument("--node", default=None, help="红外板 IP（要发命令才需要）")
    ap.add_argument("--node-port", type=int, default=8891, help="红外板监听端口，默认 8891")
    ap.add_argument("--send", default=None, help="只发这一条命令，发完退出")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.bind, args.port))
    except OSError as exc:
        print("[GW] 绑定 %s:%d 失败：%s" % (args.bind, args.port, exc))
        print("     端口被占用？换 --port，或先关掉占着它的程序。")
        return 2

    if args.send:
        if not args.node:
            print("[GW] --send 需要同时给 --node <红外板IP>")
            return 2
        sock.sendto(args.send.encode("utf-8"), (args.node, args.node_port))
        print("[GW] -> %s:%d  %s" % (args.node, args.node_port, args.send), flush=True)
        return 0

    print("[GW] 监听 udp %s:%d，等节点上报……（Ctrl+C 退出）" % (args.bind, args.port), flush=True)

    if args.node:
        threading.Thread(target=input_loop, args=(sock, args.node, args.node_port), daemon=True).start()

    stats = {}
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            text = data.decode("utf-8", "replace").strip()
            ts = time.strftime("%H:%M:%S")
            tip = fmt_report(text)
            print("[%s] %s:%d  %s    %s" % (ts, addr[0], addr[1], text, tip), flush=True)
            src = text.split(",", 1)[0]
            stats[src] = stats.get(src, 0) + 1
    except KeyboardInterrupt:
        print("\n[GW] 收工，统计：%s" % (stats if stats else "一条都没收到"))
        print("     一条都没收到就检查：板子真烧进去了吗 / 同一个 Wi-Fi 吗 / GATEWAY_IP 填对了吗 / 防火墙放行了吗")
        return 0


if __name__ == "__main__":
    sys.exit(main())
