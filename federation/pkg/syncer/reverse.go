package syncer

import (
	"context"
	"fmt"
	"log"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"

	"github.com/arterialist/kube32/federation/pkg/client"
)

type ReverseSyncer struct {
	cfg        *SyncerConfig
	kubeClient kubernetes.Interface
	k32Client  *client.Kube32Client
	stopCh     chan struct{}
}

func NewReverseSyncer(cfg *SyncerConfig, kubeClient kubernetes.Interface, k32Client *client.Kube32Client) *ReverseSyncer {
	if cfg.SyncInterval <= 0 {
		cfg.SyncInterval = 2 * time.Second
	}
	return &ReverseSyncer{
		cfg:        cfg,
		kubeClient: kubeClient,
		k32Client:  k32Client,
		stopCh:     make(chan struct{}),
	}
}

func (s *ReverseSyncer) Start(ctx context.Context) {
	go s.runReverseLoop(ctx)
}

func (s *ReverseSyncer) Stop() {
	close(s.stopCh)
}

func (s *ReverseSyncer) ReconcileDownstreamPod(ctx context.Context, kPod *client.Kube32Pod) error {
	ns := kPod.Metadata.Namespace
	if ns == "" {
		ns = "default"
	}
	name := kPod.Metadata.Name

	upstreamPod, err := s.kubeClient.CoreV1().Pods(ns).Get(ctx, name, metav1.GetOptions{})
	if apierrors.IsNotFound(err) {
		return nil // Upstream pod doesn't exist
	} else if err != nil {
		return fmt.Errorf("get upstream pod %s/%s error: %w", ns, name, err)
	}

	if upstreamPod.Spec.NodeName != s.cfg.VirtualNodeName {
		return nil // Not managed by this virtual kubelet
	}

	// Translate downstream phase & container states
	newPhase := corev1.PodPending
	switch kPod.Status.Phase {
	case "Running":
		newPhase = corev1.PodRunning
	case "Succeeded":
		newPhase = corev1.PodSucceeded
	case "Failed":
		newPhase = corev1.PodFailed
	case "Pending":
		newPhase = corev1.PodPending
	}

	podIP := kPod.Status.PodIP
	if podIP == "" && upstreamPod.Status.PodIP != "" {
		podIP = upstreamPod.Status.PodIP
	}

	containerStatuses := make([]corev1.ContainerStatus, 0)
	cName := "main"
	cImage := "wasm://workload"
	if len(upstreamPod.Spec.Containers) > 0 {
		cName = upstreamPod.Spec.Containers[0].Name
		cImage = upstreamPod.Spec.Containers[0].Image
	}

	cs := corev1.ContainerStatus{
		Name:  cName,
		Image: cImage,
	}

	switch newPhase {
	case corev1.PodRunning:
		cs.Ready = true
		cs.State = corev1.ContainerState{
			Running: &corev1.ContainerStateRunning{
				StartedAt: metav1.Now(),
			},
		}
	case corev1.PodSucceeded:
		cs.Ready = false
		cs.State = corev1.ContainerState{
			Terminated: &corev1.ContainerStateTerminated{
				ExitCode: 0,
				Reason:   "Completed",
			},
		}
	case corev1.PodFailed:
		cs.Ready = false
		exitCode := int32(137) // Standard OOM / runtime trap code
		reason := "RuntimeTrap"
		if len(kPod.Status.ContainerStatuses) > 0 {
			if term, ok := kPod.Status.ContainerStatuses[0].State["terminated"].(map[string]interface{}); ok {
				if ec, exists := term["exitCode"].(float64); exists {
					exitCode = int32(ec)
				}
				if r, exists := term["reason"].(string); exists {
					reason = r
				}
			}
		}
		if kPod.Status.Reason != "" {
			reason = kPod.Status.Reason
		}
		cs.State = corev1.ContainerState{
			Terminated: &corev1.ContainerStateTerminated{
				ExitCode: exitCode,
				Reason:   reason,
				Message:  kPod.Status.Message,
			},
		}
	case corev1.PodPending:
		cs.Ready = false
		cs.State = corev1.ContainerState{
			Waiting: &corev1.ContainerStateWaiting{
				Reason: "ContainerCreating",
			},
		}
	}

	containerStatuses = append(containerStatuses, cs)

	// Check if status update is necessary
	needsUpdate := false
	if upstreamPod.Status.Phase != newPhase {
		needsUpdate = true
	}
	if podIP != "" && upstreamPod.Status.PodIP != podIP {
		needsUpdate = true
	}
	if len(upstreamPod.Status.ContainerStatuses) != len(containerStatuses) {
		needsUpdate = true
	} else if len(containerStatuses) > 0 {
		cur := upstreamPod.Status.ContainerStatuses[0]
		if cur.Ready != cs.Ready {
			needsUpdate = true
		}
		if (cur.State.Terminated != nil && cs.State.Terminated != nil && cur.State.Terminated.ExitCode != cs.State.Terminated.ExitCode) ||
			(cur.State.Terminated == nil && cs.State.Terminated != nil) ||
			(cur.State.Running == nil && cs.State.Running != nil) {
			needsUpdate = true
		}
	}

	if needsUpdate {
		upstreamPod.Status.Phase = newPhase
		if podIP != "" {
			upstreamPod.Status.PodIP = podIP
			upstreamPod.Status.PodIPs = []corev1.PodIP{{IP: podIP}}
		}
		upstreamPod.Status.ContainerStatuses = containerStatuses

		_, err := s.kubeClient.CoreV1().Pods(ns).UpdateStatus(ctx, upstreamPod, metav1.UpdateOptions{})
		if err != nil {
			return fmt.Errorf("update status for %s/%s error: %w", ns, name, err)
		}
		log.Printf("[REVERSE-SYNC] Updated upstream status for %s/%s -> %s (IP=%s)", ns, name, newPhase, podIP)
	}

	return nil
}

func (s *ReverseSyncer) runReverseLoop(ctx context.Context) {
	ticker := time.NewTicker(s.cfg.SyncInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-s.stopCh:
			return
		case <-ticker.C:
			pods, err := s.k32Client.GetPods(ctx, "")
			if err != nil {
				continue
			}

			for i := range pods {
				if err := s.ReconcileDownstreamPod(ctx, &pods[i]); err != nil {
					log.Printf("[REVERSE-SYNC] Reconcile error: %v", err)
				}
			}
		}
	}
}
