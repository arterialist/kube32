package ipc

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"testing"
	"time"
)

func TestGatewaySendAndRecv(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	udpPort := 14242
	httpPort := 18443
	gw := NewChannelGateway(udpPort, httpPort)
	if err := gw.Start(ctx); err != nil {
		t.Fatalf("Failed to start gateway: %v", err)
	}
	defer gw.Stop()

	// Wait for servers to be listening
	time.Sleep(100 * time.Millisecond)

	// Send message to our own UDP port using HTTP REST
	sendPayload := map[string]interface{}{
		"channel_uri": "pod://node-1/sensor",
		"target_pod":  "sensor-pod",
		"sender_pod":  "cloud-orchestrator",
		"payload":     "Telemetry-Ping-Payload",
		"dest_port":   udpPort,
	}
	body, _ := json.Marshal(sendPayload)

	resp, err := http.Post(fmt.Sprintf("http://127.0.0.1:%d/api/v1/channels/pod://node-1/sensor/send", httpPort), "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("POST send error: %v", err)
	}
	respBody, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("POST send status: %d, body: %s", resp.StatusCode, string(respBody))
	}

	// Receive message via HTTP REST
	recvResp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/api/v1/channels/pod://node-1/sensor/recv?timeout=1000ms", httpPort))
	if err != nil {
		t.Fatalf("GET recv error: %v", err)
	}
	if recvResp.StatusCode != http.StatusOK {
		t.Fatalf("GET recv status: %d", recvResp.StatusCode)
	}
	var recvData map[string]interface{}
	_ = json.NewDecoder(recvResp.Body).Decode(&recvData)
	recvResp.Body.Close()

	if recvData["payload"] != "Telemetry-Ping-Payload" {
		t.Errorf("Expected payload 'Telemetry-Ping-Payload', got %v", recvData["payload"])
	}
}
