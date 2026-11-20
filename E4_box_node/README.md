# E4 物流节点（E53-ST1）

定位稳定后短鸣两声。GPS 测得位置距基准超过 **5 米**时蜂鸣约 **3 秒**，移动用时不限。报警结束后重设基准。GPS、本地报警和每秒串口输出均可离线运行。

## 配置与烧录

编辑 [box_config.h](include/box_config.h)，填写现场 Wi-Fi、主控地址 `BOX_GATEWAY_IP` 和巡检地址 `BOX_INSPECTION_URL`。直接用电脑收包时，主控地址填电脑 IPv4。

在 SDK 的 `sample/BUILD.gn` 启用 `"emergency-station/E4_box_node:box_node"`，从 SDK 根目录运行 `python build.py BearPi-HM_Nano` 后烧录。

## 上报与蜂鸣

串口每秒输出 `[GPS] NODE=BOX,...`，联网后同时向主控 UDP **8888** 上报。

```text
NODE=BOX,GPS=1,FIX=1,LAT_E6=31123456,LON_E6=121654321,BUZ=0
```

`FIX=1` 表示定位有效，经纬度除以 1000000 得到十进制度。`BUZ` 表示当前蜂鸣状态。E4 在 UDP **8892** 接收主控发来的 `CMD=BUZZ,ON=1`（响约 3 秒）或 `CMD=BUZZ,ON=0`（停止远程蜂鸣）。使用 IA1 转发时，电脑把命令发给 IA1 的 UDP **8888**。

## NFC 巡检

在 E4 目录运行：

```powershell
python inspection/server.py --host 0.0.0.0 --port 8080
```

巡检地址为 `http://<巡检电脑IPv4>:8080/inspect?device=HDPAP00008-325JPC4X0I`。配置并烧录后，手机碰 NFC 标签打开页面，提交后显示记录编号。记录存在 `inspection/inspections.sqlite3`。地址填 `""` 时保留标签原内容。

## 待实板验收

- 室外离线定位后走离基准，确认测得位移超过 5 米时出现 `[MOVE]` 并蜂鸣。
- E4 与 IA1 接入同一局域网，确认 IA1 收到坐标，且下发命令能触发 E4 蜂鸣。
