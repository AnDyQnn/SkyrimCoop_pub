# SkyrimCoop server + web panel.
#   docker compose up -d        (from this folder), panel: http://<pc>:8080
# Consoles in the same Wi-Fi find the room by broadcast only if the container sees the LAN directly
# (Linux host: network_mode host). With Docker Desktop on Windows broadcasts do not reach the container:
# type the PC's address in the game menu ("Ввести адрес"), or run SkyrimCoopServer.exe instead.
FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir aiohttp==3.14.3
COPY *.py ./
COPY web ./web
ENV SKYCOOP_DATA=/data PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 27015/tcp 27015/udp 8080/tcp
CMD ["python", "main.py", "--no-browser", "--quiet"]
