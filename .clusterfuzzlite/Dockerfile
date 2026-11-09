# ClusterFuzzLite / OSS-Fuzz build image for the SIAARA fuzz targets (fuzz/targets/).
# Base image pinned by digest (gcr.io/oss-fuzz-base/base-builder:latest as of 2026-11-09).
FROM gcr.io/oss-fuzz-base/base-builder@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build python3 zip && rm -rf /var/lib/apt/lists/*
COPY . $SRC/siaara-dicom
WORKDIR $SRC/siaara-dicom
COPY .clusterfuzzlite/build.sh $SRC/
