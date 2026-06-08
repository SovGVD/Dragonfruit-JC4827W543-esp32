# ESP-IDF toolchain for building the M5Paper Palm IIIx emulator firmware.
#
# Based on the official Espressif image, which bundles ESP-IDF + the xtensa
# toolchain and an entrypoint that sources $IDF_PATH/export.sh before running
# the given command (so `idf.py ...` just works).
#
# Build toolchain: a stable ESP-IDF release branch. Keep the image tag in
# docker-compose.yml in sync when bumping; pin a specific espressif/idf:vX.Y.Z for
# byte-reproducible builds.
FROM espressif/idf:release-v6.0

# Place to add extra build-time apt packages later, e.g.:
#   RUN apt-get update && apt-get install -y --no-install-recommends <pkg> \
#       && rm -rf /var/lib/apt/lists/*

# The base image already sets ENTRYPOINT to source the IDF environment.
# Default to a build; override on the command line (e.g. `idf.py fullclean`).
CMD ["idf.py", "build"]
