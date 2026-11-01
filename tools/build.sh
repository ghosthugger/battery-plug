#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
env -u UV_INDEX -u UV_DEFAULT_INDEX -u UV_INDEX_URL -u PIP_INDEX_URL \
  uv --no-config tool run --default-index https://pypi.org/simple \
  --with cmake --with ninja --from esphome==2026.10.1 esphome compile battery-plug.yaml
