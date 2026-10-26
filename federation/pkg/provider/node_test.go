package provider

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/arterialist/kube32/federation/pkg/client"
)

func TestAggregateCapacity(t *testing.T) {
	mockNodes := client.Kube32NodeList{
		Kind: "NodeList",
		Items: []client.Kube32Node{
			{
				APIVersion: "v1",
				Kind:       "Node",
				Metadata: struct {
					Name   string            `json:"name"`
					Labels map[string]string `json:"labels,omitempty"`
				}{
					Name: "esp32-node-01",
				},
				Status: client.NodeStatus{
					Capacity: map[string]string{
						"memory":       "320Ki",
						"cpu":          "2",
						"esp32.io/gpio": "16",
						"esp32.io/i2c":  "2",
						"esp32.io/adc":  "4",
						"esp32.io/psram": "2Mi",
					},
					Allocatable: map[string]string{
						"memory":       "240Ki",
						"cpu":          "2",
						"esp32.io/gpio": "14",
						"esp32.io/i2c":  "2",
						"esp32.io/adc":  "4",
						"esp32.io/psram": "1920Ki",
					},
					Conditions: []client.NodeCondition{
						{Type: "Ready", Status: "True"},
					},
				},
			},
			{
				APIVersion: "v1",
				Kind:       "Node",
				Metadata: struct {
					Name   string            `json:"name"`
					Labels map[string]string `json:"labels,omitempty"`
				}{
					Name: "esp32-node-02",
				},
				Status: client.NodeStatus{
					Capacity: map[string]string{
						"memory":       "320Ki",
						"cpu":          "2",
						"esp.io/gpio":  "16",
						"esp.io/i2c":   "2",
						"esp.io/adc":   "4",
						"esp.io/psram": "2Mi",
					},
					Allocatable: map[string]string{
						"memory":       "240Ki",
						"cpu":          "2",
						"esp.io/gpio":  "14",
						"esp.io/i2c":   "2",
						"esp.io/adc":   "4",
						"esp.io/psram": "1920Ki",
					},
					Conditions: []client.NodeCondition{
						{Type: "Ready", Status: "True"},
					},
				},
			},
		},
	}

	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/nodes" {
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(mockNodes)
			return
		}
		http.NotFound(w, r)
	}))
	defer ts.Close()

	k32Client := client.NewKube32Client(ts.URL)
	ctrl := NewNodeController(&NodeConfig{
		NodeName:      "test-gateway",
		LeaseInterval: 10 * time.Second,
		SyncInterval:  2 * time.Second,
	}, nil, k32Client)

	capList, allocList, anyReady, err := ctrl.AggregateCapacity(context.Background())
	if err != nil {
		t.Fatalf("AggregateCapacity failed: %v", err)
	}
	if !anyReady {
		t.Errorf("Expected anyReady=true, got false")
	}

	// Total CPU: 2 + 2 = 4
	cpuCap := capList.Cpu().Value()
	if cpuCap != 4 {
		t.Errorf("Expected 4 CPUs, got %d", cpuCap)
	}

	// Total Allocatable Memory: (240Ki + 1920Ki) * 2 = 2160Ki * 2 = 4320Ki = 4423680 bytes
	allocMem := allocList.Memory().Value()
	expectedAllocMem := int64(2 * (240*1024 + 1920*1024))
	if allocMem != expectedAllocMem {
		t.Errorf("Expected allocatable memory %d, got %d", expectedAllocMem, allocMem)
	}

	// Total Allocatable GPIO: 14 + 14 = 28
	gpioQty := allocList["esp32.io/gpio"]
	if gpioQty.Value() != 28 {
		t.Errorf("Expected 28 GPIOs, got %d", gpioQty.Value())
	}
}
