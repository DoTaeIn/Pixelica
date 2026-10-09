# Pixelica online relay. Standard library only; no pip install step.
FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIXELICA_DB=/data/online.sqlite3

RUN useradd --system --uid 10001 --no-create-home relay \
 && mkdir /data && chown relay /data

WORKDIR /app
COPY server.py /app/server.py

USER relay
VOLUME ["/data"]
EXPOSE 8080
STOPSIGNAL SIGTERM

HEALTHCHECK --interval=30s --timeout=4s --start-period=5s --retries=3 \
  CMD ["python", "-c", "import urllib.request as u; u.urlopen('http://127.0.0.1:8080/health', timeout=3).read()"]

# Binds all container interfaces; the compose file publishes NO host port for it.
# Only Caddy (HTTPS) is reachable from outside.
ENTRYPOINT ["python", "-I", "/app/server.py"]
CMD ["--host", "0.0.0.0", "--port", "8080"]
