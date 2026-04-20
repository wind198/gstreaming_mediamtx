# MediaMTX + publisher (Rust GStreamer WHIP default; optional C++ WHIP).
.DEFAULT_GOAL := help

COMPOSE       ?= docker compose -f docker-compose.mediamtx.yml
PROFILE_PUBLISH ?= --profile publish
PROFILE_STABLE ?= --profile publish-stable
PROFILE_CPP   ?= --profile publish-cpp
TIMEOUT       ?= 45

.PHONY: help up down ps logs build build-cpp publish-up publish-down publish-stable-up \
	publish-cpp-up test-api test-video-whip-seek check-video test-all

# Run `make` with no arguments to see help.

help: ## Show available targets
	@echo "MediaMTX / publisher targets (run from repo root):"
	@echo ""
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  %-20s %s\n", $$1, $$2}'
	@echo ""
	@echo "Variables: TIMEOUT=$(TIMEOUT) (seconds for timed publisher tests)"

up: ## Start MediaMTX only (detached)
	$(COMPOSE) up -d mediamtx

down: ## Stop all services started by this compose project
	$(COMPOSE) down

ps: ## Show compose service status
	$(COMPOSE) ps -a

logs: ## Tail MediaMTX logs (fleet_mediamtx_dev)
	docker logs -f fleet_mediamtx_dev

build: ## Build whip_gstreamer image (Rust whip-publish + gst-plugins-rs)
	$(COMPOSE) $(PROFILE_PUBLISH) build whip_video

build-cpp: ## Build whip_cpp_gstreamer image (C++ WHIP binary + gst-plugins-rs)
	docker build -f Dockerfile.cpp_whip -t whip_cpp_gstreamer:local .

publish-up: ## Start MediaMTX + Rust WHIP publisher on path `live` (`publish` profile)
	$(COMPOSE) $(PROFILE_PUBLISH) up -d --build

publish-stable-up: ## Same as publish-up (alias)
	$(COMPOSE) $(PROFILE_STABLE) up -d --build

publish-down: ## Stop all services in this compose project
	$(COMPOSE) down

publish-cpp-up: ## Start MediaMTX + C++ WHIP publisher (`publish-cpp` profile only)
	$(COMPOSE) $(PROFILE_CPP) up -d --build

check-video: ## Exit 1 if ./test.mp4 is missing (compose bind-mount)
	@test -f ./test.mp4 || (echo "Missing ./test.mp4 — add the file or change docker-compose.mediamtx.yml volume." >&2; exit 1)

test-api: ## GET MediaMTX API paths list (expects mediamtx up)
	curl -sf http://127.0.0.1:8888/v3/paths/list | sed 's/,/\n/g' | head -20 || true

test-video-whip-seek: check-video ## Timed Rust WHIP publish test (path live); needs mediamtx + ./test.mp4
	$(COMPOSE) $(PROFILE_PUBLISH) run --rm whip_video \
		timeout $(TIMEOUT) /app/whip-publish

test-all: test-api test-video-whip-seek ## Run API check then timed Rust WHIP test (set TIMEOUT=60 for longer)
