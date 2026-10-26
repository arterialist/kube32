.PHONY: all build test clean lint

MODULE=github.com/arterialist/kube32/federation
BIN_DIR=bin
BINARY_NAME=virtual-kubelet

all: build test

build:
	mkdir -p $(BIN_DIR)
	go build -ldflags="-s -w" -o $(BIN_DIR)/$(BINARY_NAME) ./cmd/virtual-kubelet

test:
	go test -v ./...

clean:
	rm -rf $(BIN_DIR)

lint:
	go vet ./...
