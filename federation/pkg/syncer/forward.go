package syncer

import (
	"context"
	"fmt"
	"log"
	"sync"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"

	"github.com/arterialist/kube32/federation/pkg/client"
)

const (
	FederationFinalizer = "kube32.io/federation-protection"
)

type SyncerConfig struct {
	VirtualNodeName string
	SyncInterval    time.Duration
}

type ForwardSyncer struct {
	cfg        *SyncerConfig
	kubeClient kubernetes.Interface
	k32Client  *client.Kube32Client

	submittedPods map[string]bool // ns/name -> true
	mu            sync.RWMutex
	stopCh        chan struct{}
}

func NewForwardSyncer(cfg *SyncerConfig, kubeClient kubernetes.Interface, k32Client *client.Kube32Client) *ForwardSyncer {
	if cfg.SyncInterval <= 0 {
		cfg.SyncInterval = 2 * time.Second
	}
	return &ForwardSyncer{
		cfg:           cfg,
		kubeClient:    kubeClient,
		k32Client:     k32Client,
		submittedPods: make(map[string]bool),
		stopCh:        make(chan struct{}),
	}
}

func (s *ForwardSyncer) Start(ctx context.Context) {
	go s.runForwardLoop(ctx)
}

func (s *ForwardSyncer) Stop() {
	close(s.stopCh)
}

func (s *ForwardSyncer) TranslatePodSpec(upstreamPod *corev1.Pod) map[string]interface{} {
	labels := make(map[string]string)
	if upstreamPod.Labels != nil {
		for k, v := range upstreamPod.Labels {
			labels[k] = v
		}
	}
	labels["federated-from"] = "upstream-k8s-cluster"

	annotations := make(map[string]string)
	if upstreamPod.Annotations != nil {
		for k, v := range upstreamPod.Annotations {
			annotations[k] = v
		}
	}

	containers := make([]map[string]interface{}, 0, len(upstreamPod.Spec.Containers))
	for _, c := range upstreamPod.Spec.Containers {
		cMap := map[string]interface{}{
			"name":  c.Name,
			"image": c.Image,
		}

		resLimits := make(map[string]string)
		for rName, rQty := range c.Resources.Limits {
			resLimits[string(rName)] = rQty.String()
		}
		resRequests := make(map[string]string)
		for rName, rQty := range c.Resources.Requests {
			resRequests[string(rName)] = rQty.String()
		}

		cMap["resources"] = map[string]interface{}{
			"limits":   resLimits,
			"requests": resRequests,
		}
		containers = append(containers, cMap)
	}

	manifest := map[string]interface{}{
		"apiVersion": "v1",
		"kind":       "Pod",
		"metadata": map[string]interface{}{
			"name":        upstreamPod.Name,
			"namespace":   upstreamPod.Namespace,
			"labels":      labels,
			"annotations": annotations,
		},
		"spec": map[string]interface{}{
			// Crucial: spec.nodeName is stripped so Kube32 autonomous scheduler binds it to physical ESP32
			"nodeName":   "",
			"containers": containers,
		},
	}
	return manifest
}

func (s *ForwardSyncer) ReconcileUpstreamPod(ctx context.Context, pod *corev1.Pod) error {
	log.Printf("[FORWARD-SYNC] ReconcileUpstreamPod: %s/%s, nodeName=%q (target=%q)", pod.Namespace, pod.Name, pod.Spec.NodeName, s.cfg.VirtualNodeName)
	if pod.Spec.NodeName != s.cfg.VirtualNodeName {
		return nil
	}

	// Deletions are processed by DeletionSyncer
	if pod.DeletionTimestamp != nil {
		return nil
	}

	key := fmt.Sprintf("%s/%s", pod.Namespace, pod.Name)

	// 1. Ensure Federation Finalizer is present upstream
	hasFinalizer := false
	for _, f := range pod.Finalizers {
		if f == FederationFinalizer {
			hasFinalizer = true
			break
		}
	}

	if !hasFinalizer {
		podCopy := pod.DeepCopy()
		podCopy.Finalizers = append(podCopy.Finalizers, FederationFinalizer)
		updatedPod, err := s.kubeClient.CoreV1().Pods(pod.Namespace).Update(ctx, podCopy, metav1.UpdateOptions{})
		if err != nil {
			return fmt.Errorf("failed to attach finalizer to %s: %w", key, err)
		}
		pod = updatedPod
	}

	// 2. Check if already dispatched to downstream Kube32
	downstreamPod, err := s.k32Client.GetPod(ctx, pod.Namespace, pod.Name)
	if err != nil {
		return fmt.Errorf("check downstream pod %s error: %w", key, err)
	}

	if downstreamPod == nil {
		// Forward pod to Kube32
		log.Printf("[FORWARD-SYNC] Calling CreatePod on Kube32 for %s", key)
		manifest := s.TranslatePodSpec(pod)
		created, err := s.k32Client.CreatePod(ctx, pod.Namespace, manifest)
		if err != nil {
			log.Printf("[FORWARD-SYNC] CreatePod on Kube32 failed for %s: %v", key, err)
			return fmt.Errorf("create downstream pod %s error: %w", key, err)
		}
		log.Printf("[FORWARD-SYNC] Dispatched pod %s to Kube32 (phase=%s)", key, created.Status.Phase)
		s.mu.Lock()
		s.submittedPods[key] = true
		s.mu.Unlock()
	} else {
		log.Printf("[FORWARD-SYNC] Downstream pod %s already exists (phase=%s)", key, downstreamPod.Status.Phase)
	}

	return nil
}

func (s *ForwardSyncer) runForwardLoop(ctx context.Context) {
	ticker := time.NewTicker(s.cfg.SyncInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-s.stopCh:
			return
		case <-ticker.C:
			pods, err := s.kubeClient.CoreV1().Pods("").List(ctx, metav1.ListOptions{})
			if err != nil {
				log.Printf("[FORWARD-SYNC] Failed to list upstream pods: %v", err)
				continue
			}

			for i := range pods.Items {
				pod := &pods.Items[i]
				if err := s.ReconcileUpstreamPod(ctx, pod); err != nil {
					log.Printf("[FORWARD-SYNC] Reconcile error for %s/%s: %v", pod.Namespace, pod.Name, err)
				}
			}
		}
	}
}
