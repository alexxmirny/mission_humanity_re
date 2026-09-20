# src/collector/Dockerfile -- the report collector (tracker dist:RP2, plan D14).
#
# Build (from the repo root, so the build context is this directory):
#   docker build -t mh-collector:local src/collector
# Normally built via docker-compose.yml instead (`docker compose up`).
#
# UNVERIFIED on this machine: Docker is not installed here (2026-09-17). The
# service itself (this image's CMD) is proven directly with uvicorn + pytest;
# see README.md "What is verified vs. not" for the exact commands to run once
# Docker exists.
FROM python:3.12-slim

# --no-install-recommends keeps the image lean; no compiler toolchain is needed
# -- fastapi/starlette/uvicorn/python-multipart are all pure Python or ship
# manylinux wheels, and uvicorn[standard]'s C-accelerated extras (httptools,
# uvloop) also ship wheels for this base image's glibc + Python 3.12.
COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir --root-user-action=ignore -r /tmp/requirements.txt \
    && rm /tmp/requirements.txt

# Non-root: a fixed uid (not the next-available useradd default) so the bind
# volume's ownership is stable across image rebuilds.
RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin collector

WORKDIR /app
COPY collector /app/collector

RUN mkdir -p /data/reports && chown -R collector:collector /data /app
USER collector

ENV REPORTS_DIR=/data/reports \
    TOKEN_FILE=/run/secrets/report_token \
    MAX_BODY_MB=64 \
    RATE_PER_MIN=10 \
    TIMESTAMP_SKEW_SECONDS=600 \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

EXPOSE 8000

# `collector.app:create_app` + `--factory`: get_settings() (called inside
# create_app) reads TOKEN_FILE, which only exists once the compose secret is
# mounted -- see app.py's comment on why there is no eager module-level `app`.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8000/healthz', timeout=3).status == 200 else 1)"

CMD ["uvicorn", "collector.app:create_app", "--factory", "--host", "0.0.0.0", "--port", "8000", "--workers", "2"]
