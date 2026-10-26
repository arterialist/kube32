package syncer

import (
	"testing"

	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

func TestTranslatePodSpec(t *testing.T) {
	syncer := NewForwardSyncer(&SyncerConfig{
		VirtualNodeName: "kube32-edge-gateway",
	}, nil, nil)

	upstreamPod := &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "test-sensor-pod",
			Namespace: "default",
			Labels: map[string]string{
				"app": "sensor",
			},
		},
		Spec: corev1.PodSpec{
			NodeName: "kube32-edge-gateway",
			Containers: []corev1.Container{
				{
					Name:  "sensor-worker",
					Image: "wasm://sensor:v1",
					Resources: corev1.ResourceRequirements{
						Limits: corev1.ResourceList{
							corev1.ResourceMemory:           resource.MustParse("64Ki"),
							corev1.ResourceName("esp32.io/gpio"): resource.MustParse("1"),
						},
						Requests: corev1.ResourceList{
							corev1.ResourceMemory: resource.MustParse("32Ki"),
						},
					},
				},
			},
		},
	}

	manifest := syncer.TranslatePodSpec(upstreamPod)

	meta, ok := manifest["metadata"].(map[string]interface{})
	if !ok || meta["name"] != "test-sensor-pod" {
		t.Fatalf("Expected metadata.name 'test-sensor-pod', got %v", meta["name"])
	}

	labels, ok := meta["labels"].(map[string]string)
	if !ok || labels["federated-from"] != "upstream-k8s-cluster" {
		t.Errorf("Missing federated-from label in translated manifest")
	}

	spec, ok := manifest["spec"].(map[string]interface{})
	if !ok {
		t.Fatalf("Missing spec in manifest")
	}

	// Crucial assertion: spec.nodeName MUST be empty string so Kube32 scheduler can autonomously bind it!
	if spec["nodeName"] != "" {
		t.Errorf("Expected spec.nodeName to be stripped (empty string), got %v", spec["nodeName"])
	}

	containers, ok := spec["containers"].([]map[string]interface{})
	if !ok || len(containers) != 1 {
		t.Fatalf("Expected 1 container in translated spec, got %v", containers)
	}

	res, ok := containers[0]["resources"].(map[string]interface{})
	if !ok {
		t.Fatalf("Missing resources in container")
	}

	limits, ok := res["limits"].(map[string]string)
	if !ok || limits["memory"] != "64Ki" {
		t.Errorf("Expected memory limit '64Ki', got %v", limits["memory"])
	}
	if limits["esp32.io/gpio"] != "1" {
		t.Errorf("Expected gpio limit '1', got %v", limits["esp32.io/gpio"])
	}
}
