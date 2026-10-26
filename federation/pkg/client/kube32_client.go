package client

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

type NodeCapacity struct {
	Memory string `json:"memory"`
	CPU    string `json:"cpu"`
	GPIO   string `json:"esp32.io/gpio,omitempty"`
	I2C    string `json:"esp32.io/i2c,omitempty"`
	ADC    string `json:"esp32.io/adc,omitempty"`
	PSRAM  string `json:"esp32.io/psram,omitempty"`

	// Aliases for esp.io/*
	AltGPIO  string `json:"esp.io/gpio,omitempty"`
	AltI2C   string `json:"esp.io/i2c,omitempty"`
	AltADC   string `json:"esp.io/adc,omitempty"`
	AltPSRAM string `json:"esp.io/psram,omitempty"`
}

type NodeCondition struct {
	Type               string `json:"type"`
	Status             string `json:"status"`
	LastHeartbeatTime  string `json:"lastHeartbeatTime,omitempty"`
	LastTransitionTime string `json:"lastTransitionTime,omitempty"`
	Reason             string `json:"reason,omitempty"`
	Message            string `json:"message,omitempty"`
}

type NodeStatus struct {
	Capacity    map[string]string `json:"capacity"`
	Allocatable map[string]string `json:"allocatable"`
	Conditions  []NodeCondition   `json:"conditions"`
}

type Kube32Node struct {
	APIVersion string `json:"apiVersion"`
	Kind       string `json:"kind"`
	Metadata   struct {
		Name   string            `json:"name"`
		Labels map[string]string `json:"labels,omitempty"`
	} `json:"metadata"`
	Status NodeStatus `json:"status"`
}

type Kube32NodeList struct {
	Kind  string       `json:"kind"`
	Items []Kube32Node `json:"items"`
}

type ContainerStatus struct {
	Name         string                 `json:"name"`
	Ready        bool                   `json:"ready"`
	RestartCount int                    `json:"restartCount"`
	Image        string                 `json:"image"`
	State        map[string]interface{} `json:"state"`
}

type Kube32PodStatus struct {
	Phase             string            `json:"phase"`
	PodIP             string            `json:"podIP,omitempty"`
	HostIP            string            `json:"hostIP,omitempty"`
	Message           string            `json:"message,omitempty"`
	Reason            string            `json:"reason,omitempty"`
	ContainerStatuses []ContainerStatus `json:"containerStatuses,omitempty"`
}

type Kube32Pod struct {
	APIVersion string `json:"apiVersion"`
	Kind       string `json:"kind"`
	Metadata   struct {
		Name              string            `json:"name"`
		Namespace         string            `json:"namespace"`
		UID               string            `json:"uid,omitempty"`
		Labels            map[string]string `json:"labels,omitempty"`
		Annotations       map[string]string `json:"annotations,omitempty"`
		DeletionTimestamp string            `json:"deletionTimestamp,omitempty"`
	} `json:"metadata"`
	Spec   map[string]interface{} `json:"spec"`
	Status Kube32PodStatus        `json:"status"`
}

type Kube32PodList struct {
	Kind  string      `json:"kind"`
	Items []Kube32Pod `json:"items"`
}

type Kube32Client struct {
	baseURL    string
	httpClient *http.Client
}

func NewKube32Client(baseURL string) *Kube32Client {
	return &Kube32Client{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: 5 * time.Second,
		},
	}
}

func (c *Kube32Client) CheckHealth(ctx context.Context) (bool, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, fmt.Sprintf("%s/version", c.baseURL), nil)
	if err != nil {
		return false, err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return false, err
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK, nil
}

func (c *Kube32Client) GetNodes(ctx context.Context) ([]Kube32Node, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, fmt.Sprintf("%s/api/v1/nodes", c.baseURL), nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("get nodes failed: code %d, body: %s", resp.StatusCode, string(body))
	}

	var nodeList Kube32NodeList
	if err := json.NewDecoder(resp.Body).Decode(&nodeList); err != nil {
		return nil, fmt.Errorf("decode nodes failed: %w", err)
	}
	return nodeList.Items, nil
}

func (c *Kube32Client) GetPods(ctx context.Context, namespace string) ([]Kube32Pod, error) {
	url := fmt.Sprintf("%s/api/v1/pods", c.baseURL)
	if namespace != "" {
		url = fmt.Sprintf("%s/api/v1/namespaces/%s/pods", c.baseURL, namespace)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("get pods failed: code %d, body: %s", resp.StatusCode, string(body))
	}

	var podList Kube32PodList
	if err := json.NewDecoder(resp.Body).Decode(&podList); err != nil {
		return nil, fmt.Errorf("decode pods failed: %w", err)
	}
	return podList.Items, nil
}

func (c *Kube32Client) GetPod(ctx context.Context, namespace, name string) (*Kube32Pod, error) {
	url := fmt.Sprintf("%s/api/v1/namespaces/%s/pods/%s", c.baseURL, namespace, name)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return nil, nil
	}
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("get pod failed: code %d, body: %s", resp.StatusCode, string(body))
	}

	var pod Kube32Pod
	if err := json.NewDecoder(resp.Body).Decode(&pod); err != nil {
		return nil, fmt.Errorf("decode pod failed: %w", err)
	}
	return &pod, nil
}

func (c *Kube32Client) CreatePod(ctx context.Context, namespace string, podManifest map[string]interface{}) (*Kube32Pod, error) {
	url := fmt.Sprintf("%s/api/v1/namespaces/%s/pods", c.baseURL, namespace)
	data, err := json.Marshal(podManifest)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusCreated && resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("create pod failed: code %d, body: %s", resp.StatusCode, string(body))
	}

	var createdPod Kube32Pod
	if err := json.NewDecoder(resp.Body).Decode(&createdPod); err != nil {
		return nil, fmt.Errorf("decode created pod failed: %w", err)
	}
	return &createdPod, nil
}

func (c *Kube32Client) DeletePod(ctx context.Context, namespace, name string) error {
	url := fmt.Sprintf("%s/api/v1/namespaces/%s/pods/%s", c.baseURL, namespace, name)
	req, err := http.NewRequestWithContext(ctx, http.MethodDelete, url, nil)
	if err != nil {
		return err
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusNotFound && resp.StatusCode != http.StatusNoContent {
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("delete pod failed: code %d, body: %s", resp.StatusCode, string(body))
	}
	return nil
}
