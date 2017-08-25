FROM ubuntu:xenial

ENV DEBIAN_FRONTEND=noninteractive

RUN mkdir /build

RUN apt-get update && apt-get install -y --force-yes \
    gcc-4.8 \
    libc6 \
    build-essential \
    liblockfile-dev \
    zlib1g-dev \
    git \
    python-pip \
    python-dev \
    devscripts \
    debhelper

WORKDIR "/build"
CMD git submodule init && git submodule update && make test && sh docker_build/docker_build_deb.sh
