#!/usr/bin/env sh
docker build -t 4grep-docker docker_build
docker run --rm -v $(pwd):/build \
	-e COMMIT_COUNT="$(git rev-list HEAD --count)" \
	-e COMMIT_HASH="$(git rev-parse HEAD)" \
	4grep-docker
docker rmi 4grep-docker
