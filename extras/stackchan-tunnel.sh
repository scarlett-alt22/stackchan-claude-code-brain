#!/bin/bash
# StackChan 反向隧道（launchd com.example.stackchan-tunnel 拉起）。
# 2026-10-05：给日志加时间戳。ssh 本身的输出不带时间，在英国断了查不出几点断的。
# exec 让 ssh 顶替本进程成为 launchd 看管的那个 pid（launchd 停任务时直接停到 ssh，不会留孤儿占着远端端口）；
# 它的输出经过下面的小循环逐行加时间。
# 每次（重新）连都会打一行 starting，断线时间就看相邻两行 starting（ServerAlive 15s×3，约 45s 判断线＋10s 节流）。
# 没有 "ssh exited" 行：试过，ssh 一退出 launchd 就把整组进程（含这个小循环）清掉，那行来不及写（10-05 kill 实测）。
ts() { date '+%Y-%m-%d %H:%M:%S'; }
echo "$(ts) [tunnel] starting ssh (pid $$)"
exec /usr/bin/ssh -N \
  -o BatchMode=yes -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o ConnectTimeout=15 \
  -R 127.0.0.1:18124:127.0.0.1:8124 your-vps \
  > >(while IFS= read -r l; do echo "$(ts) $l"; done) 2>&1
