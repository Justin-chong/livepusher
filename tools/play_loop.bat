@echo off
rem 循环拉流：ffplay 退出后（推流断了）等 1 秒自动重连
:loop
ffplay -fflags nobuffer -flags low_delay -probesize 32 -sync ext rtmp://127.0.0.1:1935/live/movie
echo [%time%] stream ended, reconnecting in 1s...
timeout /t 1 /nobreak >nul
goto loop
